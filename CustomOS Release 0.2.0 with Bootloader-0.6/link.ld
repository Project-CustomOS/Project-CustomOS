/*
 * CustomOS kernel layout.
 *
 * The bootloader copies kernel.bin to KERNEL_BASE and jumps to its first byte,
 * so the code must be linked for exactly that address. Keep KERNEL_BASE and
 * KERNEL_MAX_SIZE in sync with KERNEL_LOAD_ADDRESS / KERNEL_MAX_PAGES in
 * bootloader.c.
 *
 * Compile.cmd links this as a PE file, and the PE linker adds --image-base to
 * every address. --image-base must therefore be 0 so that KERNEL_BASE really
 * is the final address; Compile.cmd verifies it by checking _start with nm.
 */
KERNEL_BASE     = 0x100000;
KERNEL_MAX_SIZE = 0x100000;     /* 1 MB window, includes .bss */

SECTIONS
{
    . = KERNEL_BASE;

    .text : ALIGN(16) {
        kernel_entry.o(.text)
        *(.text*)
    }

    /* MinGW puts read-only data (every L"..." string) in .rdata, ELF in .rodata */
    .rodata : ALIGN(16) {
        *(.rodata*)
        *(.rdata*)
    }

    .data : ALIGN(16) {
        *(.data*)
    }

    .bss : ALIGN(16) {
        __bss_start = .;
        *(COMMON)
        *(.bss*)
        . = ALIGN(16);
        __bss_end = .;
    }

    /DISCARD/ : {
        *(.eh_frame*)
        *(.pdata*)
        *(.xdata*)
        *(.comment)
        *(.note*)
    }
}

ASSERT(_start == KERNEL_BASE, "kernel entry (_start) must be linked at KERNEL_BASE, the first byte of kernel.bin")
ASSERT(__bss_end - KERNEL_BASE <= KERNEL_MAX_SIZE, "kernel image including .bss is larger than the 1 MB kernel window")
