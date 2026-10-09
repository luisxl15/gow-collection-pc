/* Toolchain smoke test: freestanding Vita eboot that draws text and exits. */
#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>
#include "../../runtime/vita/vita_console.h"

int vita_main(void) {
  if (vcon_init() < 0) return 1;
  vcon_puts("vita-recomp-lab: toolchain smoke test\n");
  vcon_puts("Press START to exit.\n");
  SceCtrlData pad;
  for (int frames = 0; frames < 60 * 30; frames++) {
    sceCtrlPeekBufferPositive(0, &pad, 1);
    if (pad.buttons & SCE_CTRL_START) break;
    sceKernelDelayThread(16666);
  }
  return 0;
}
