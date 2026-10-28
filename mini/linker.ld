/* BoltOS Mini: one image, linked where both loaders put it -- RAM base
 * (0x40000000 on the Orange Pi and on QEMU virt) plus the 2 MiB text_offset
 * in the Image header. Physical == virtual. */
ENTRY(_start)

SECTIONS
{
    . = 0x40200000;
    __image_start = .;
    .text : {
        KEEP(*(.text.head))
        *(.text .text.*)
    }
    .rodata : ALIGN(16) { *(.rodata .rodata.*) }
    .data : ALIGN(16) { *(.data .data.*) }
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        . += 0x40000;                   /* 256 KiB stack                   */
        __stack_top = .;
        __bss_end = .;
    }
    __image_end = .;
    __image_size = __image_end - __image_start;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
