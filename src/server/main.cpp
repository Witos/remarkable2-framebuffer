﻿#include <fcntl.h>

#include "../shared/ipc.cpp"
#include "../shared/swtfb.cpp"

#include <linux/input.h>
#include <sys/poll.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cstdio>
#include <dlfcn.h>
#include <mutex>
#include <stdint.h>
#include <semaphore.h>
#include <thread>

#include <QCoreApplication>
#include <QTouchEvent>
#include <systemd/sd-daemon.h>

using namespace swtfb;

int msg_q_id = 0x2257c;
ipc::Queue MSGQ(msg_q_id);

const int BYTES_PER_PIXEL = sizeof(uint16_t);

uint16_t *shared_mem;

void doUpdate(SwtFB &fb, const swtfb_update &s) {
  auto mxcfb_update = s.mdata.update;
  auto rect = mxcfb_update.update_region;

#ifdef DEBUG_DIRTY
  std::cerr << "Dirty Region: " << rect.left << " " << rect.top << " "
            << rect.width << " " << rect.height << endl;
#endif

  // There are three update modes on the rm2. But they are mapped to the five
  // rm1 modes as follows:
  //
  // 0: init (same as GL16)
  // 1: DU - direct update, fast
  // 2: GC16 - high fidelity (slow)
  // 3: GL16 - what the rm is using
  // 8: highlight (same as high fidelity)

  int waveform = mxcfb_update.waveform_mode;
  if (waveform > 3 && waveform != 8) {
    waveform = 3;
    fb.ClearGhosting();
  }

  if (waveform < 0) {
    waveform = 3;
    fb.ClearGhosting();
  }

  // full = 1, partial = 0
  auto update_mode = mxcfb_update.update_mode;

  auto flags = update_mode & 0x1;

  // TODO: Get sync from client (wait for second ioctl? or look at stack?)
  // There are only two occasions when the original rm1 library sets sync to
  // true. Currently we detect them by the other data. Ideally we should
  // correctly handle the corresponding ioctl (empty rect and flags == 2?).
  if (waveform == /*init*/ 0 && update_mode == /* full */ 1) {
    flags |= 2;
    std::cerr << "SERVER: sync" << std::endl;
  } else if (rect.left == 0 && rect.top > 1800 &&
             waveform == /* grayscale */ 3 && update_mode == /* full */ 1) {
    std::cerr << "server sync, x2: " << rect.width << " y2: " << rect.height
              << std::endl;
    flags |= 2;
  }

  if (waveform == /* fast */ 1 && update_mode == /* partial */ 0) {
    // fast draw
    flags = 4;
  }

#ifdef DEBUG
  std::cerr << "doUpdate " << endl;
  cerr << "mxc: waveform_mode " << mxcfb_update.waveform_mode << endl;
  cerr << "mxc: update mode " << mxcfb_update.update_mode << endl;
  cerr << "mxc: update marker " << mxcfb_update.update_marker << endl;
  cerr << "final: waveform " << waveform;
  cerr << " flags " << flags << endl << endl;
#endif

  fb.DrawRaw(rect.left, rect.top, rect.width, rect.height, waveform, flags);
}

extern "C" {
// QImage(width, height, format)
static void (*qImageCtor)(void *that, int x, int y, int f) = 0;
// QImage(uchar*, width, height, bytesperline, format)
static void (*qImageCtorWithBuffer)(void *that, uint8_t *, int32_t x, int32_t y,
                                    int32_t bytes, int format, void (*)(void *),
                                    void *) = 0;

static void _libhook_init() __attribute__((constructor));
static void _libhook_init() {
  qImageCtor = (void (*)(void *, int, int, int))dlsym(
      RTLD_NEXT, "_ZN6QImageC1EiiNS_6FormatE");
  qImageCtorWithBuffer = (void (*)(
      void *, uint8_t *, int32_t, int32_t, int32_t, int, void (*)(void *),
      void *))dlsym(RTLD_NEXT, "_ZN6QImageC1EPhiiiNS_6FormatEPFvPvES2_");
}

bool FIRST_ALLOC = true;
void _ZN6QImageC1EiiNS_6FormatE(void *that, int x, int y, int f) {
  if (x == WIDTH && y == HEIGHT && FIRST_ALLOC) {
    fprintf(stderr, "REPLACING THE IMAGE with shared memory\n");

    FIRST_ALLOC = false;
    qImageCtorWithBuffer(that, (uint8_t *)shared_mem, WIDTH, HEIGHT,
                         WIDTH * BYTES_PER_PIXEL, f, nullptr, nullptr);
    return;
  }
  qImageCtor(that, x, y, f);
}

int server_main(int, char **, char **) {
  SwtFB fb;

  if (!fb.setFunc()) {
    return 255;
  }

  shared_mem = ipc::get_shared_buffer();
  fb.initQT();

  fprintf(stderr, "WAITING FOR SEND UPDATE ON MSG Q\n");
  sd_notify(0, "READY=1");

  while (true) {
    auto buf = MSGQ.recv();
    switch (buf.mtype) {
    case ipc::UPDATE_t:
      doUpdate(fb, buf);
      break;
    case ipc::XO_t: {
      const xochitl_data& data = buf.mdata.xochitl_update;
      int width = data.x2 - data.x1 + 1;
      int height = data.y2 - data.y1 + 1;
      QRect rect(data.x1, data.y1, width, height);
      fb.SendUpdate(rect, data.waveform, data.flags);
    } break;
    case ipc::WAIT_t: {
      fb.WaitForLastUpdate();
      sem_t* sem = sem_open(buf.mdata.wait_update.sem_name, O_CREAT, 0644, 0);
      if (sem != NULL) {
        sem_post(sem);
        sem_close(sem);
      }
    } break;

    default:
      std::cerr << "Error, unknown message type" << std::endl;
    }
  }
  return 0;
}

int __libc_start_main(int (*)(int, char **, char **), int argc,
                      char **argv, int (*init)(int, char **, char **),
                      void (*fini)(void), void (*rtld_fini)(void),
                      void *stack_end) {

  printf("STARTING RM2FB\n");

  auto func_main =
      (decltype(&__libc_start_main))dlsym(RTLD_NEXT, "__libc_start_main");

  // Since we preload the library in the Xochitl binary, the process will
  // be called 'xochitl' by default. Change this to avoid confusing launchers
  const char* proc_name = "rm2fb-server";
  size_t argv0_len = strlen(argv[0]);
  strncpy(argv[0], proc_name, argv0_len);
  argv[0][argv0_len] = 0;
  prctl(PR_SET_NAME, proc_name);

  return func_main(server_main, argc, argv, init, fini, rtld_fini, stack_end);
};
};
