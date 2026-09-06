/* hvf-el1 payload: a flat image loaded at IPA 0x40000000 by the host. */
ENTRY(_start)
SECTIONS
{
    . = 0x40000000;
    .text : {
        KEEP(*(.text.start))
        *(.text.vectors)
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    .data : { *(.data .data.*) }
    . = ALIGN(4096);
    __bss_start = .;
    .bss (NOLOAD) : {
        *(.bss.pt)
        *(.bss .bss.*)
        *(COMMON)
    }
    . = ALIGN(4096);
    __bss_end = .;
    __stack_bottom = .;
    . += 0x40000;
    __stack_top = .;
    __image_end = .;
    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment) }
}
