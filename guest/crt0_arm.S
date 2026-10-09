@ Guest entry point (ARMv7, Thumb-2, static). qemu-arm / the recompiled runtime provide sp.
    .syntax unified
    .thumb
    .text
    .globl _start
    .type _start, %function
    .thumb_func
_start:
    bl      main
    bl      g_exit
1:  b       1b
    .size _start, .-_start
