/* BoltBoot boot0: linked where the boot ROM puts it, in SRAM (boot.h). */
ENTRY(_start)

SECTIONS
{
    . = 0x20000;
    .text : {
        KEEP(*(.text.head))
        *(.text .text.*)
    }
    .rodata : ALIGN(16) { *(.rodata .rodata.*) }
    .data : ALIGN(16) { *(.data .data.*) }
    __load_end = .;
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
ASSERT(__load_end <= 0x2C000, "boot0 is bigger than the 48 KiB the boot ROM loads")
ASSERT(__bss_end <= 0x50000, "boot0's .bss runs into the report at 0x50000")
