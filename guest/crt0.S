# Guest entry point (MIPS32 o32, non-PIC, static).
# Under qemu-mipsel the kernel provides $sp; under the recompiled runtime
# the host runtime initialises $sp before calling _start.
    .set noreorder
    .text
    .globl _start
    .type _start, @function
_start:
    la      $gp, _gp
    addiu   $sp, $sp, -32
    jal     main
    nop
    move    $a0, $v0
    jal     g_exit
    nop
1:  b       1b
    nop
    .size _start, .-_start
