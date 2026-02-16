/* File: src/linker.ld */
SECTIONS
{
    . = 0x80000;  /* Kernel load address for AArch64 */
    .text : { *(.text) }
    .data : { *(.data) }
    .bss  : { *(.bss) }
}