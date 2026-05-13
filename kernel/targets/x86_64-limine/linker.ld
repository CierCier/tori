OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(limine_entry)

PHDRS
{
    limine_requests PT_LOAD;
    text PT_LOAD;
    rodata PT_LOAD;
    data PT_LOAD;
}

SECTIONS
{
    . = 0xffffffff80000000;
    __kernel_virtual_start = .;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(0x1000);

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(0x1000);

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    .eh_frame : {
        __eh_frame_start = .;
        *(.eh_frame)
        __eh_frame_end = .;
    } :rodata

    .eh_frame_hdr : {
        __eh_frame_hdr_start = .;
        *(.eh_frame_hdr)
        __eh_frame_hdr_end = .;
    } :rodata

    . = ALIGN(0x1000);

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    . = ALIGN(0x1000);
    __kernel_virtual_end = .;

    /DISCARD/ : {
        *(.note .note.*)
        *(.comment)
    }
}
