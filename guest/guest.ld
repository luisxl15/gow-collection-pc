/* Guest layout: everything below 32 MiB so the recompiled runtime can map
 * guest RAM as one flat 32 MiB region (like a console's physical RAM). */
ENTRY(_start)
SECTIONS
{
  . = 0x00400000;
  .text : { *(.text.startup) *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  . = ALIGN(16);
  .data : { *(.data .data.*) }
  _gp = ALIGN(16) + 0x7ff0;
  .sdata : { *(.sdata .sdata.*) }
  .sbss : { *(.sbss .sbss.*) *(.scommon) }
  .bss : { *(.bss .bss.*) *(COMMON) }
  _end = .;
  /DISCARD/ : { *(.comment) *(.note*) *(.pdr) *(.gnu.attributes) }
}
