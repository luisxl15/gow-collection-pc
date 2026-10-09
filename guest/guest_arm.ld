/* ARM guest layout for differential tests (qemu-arm / recompiled host runtime). */
ENTRY(_start)
SECTIONS
{
  . = 0x00010000;
  .text : { *(.text.startup) *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  .ARM.exidx : { *(.ARM.exidx*) }
  . = ALIGN(4096);
  .data : { *(.data .data.*) }
  .bss : { *(.bss .bss.*) *(COMMON) }
  _end = .;
  /DISCARD/ : { *(.comment) *(.note*) *(.ARM.attributes) }
}
