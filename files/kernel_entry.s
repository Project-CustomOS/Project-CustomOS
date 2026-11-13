.global _start
.text
_start:
    # Ensure 16-byte stack alignment for the x64 ABI
    andq $-16, %rsp

    # Save incoming UEFI boot parameters (%rcx = DeviceHandle, %rdx = SystemTable)
    # in callee-saved registers; the .bss clear below needs %rcx.
    movq %rcx, %r12
    movq %rdx, %r13

    # Zero-initialize the .bss section so all uninitialized variables start at 0
    leaq __bss_start(%rip), %rdi
    leaq __bss_end(%rip), %rcx
    subq %rdi, %rcx          # Calculate .bss size in bytes
    jz .L_skip_bss

    xorq %rax, %rax          # Value to write (0)
    cld                      # Ensure string direction is forward
    rep stosb                # Zero-fill memory byte by byte

.L_skip_bss:
    # Restore UEFI parameters into the correct Microsoft x64 ABI registers
    movq %r12, %rcx          # DeviceHandle
    movq %r13, %rdx          # SystemTable

    # The Microsoft x64 ABI requires 32 bytes of "shadow space" above the
    # return address for the callee. Without it the callee can overwrite
    # whatever sits above our frame. 32 is a multiple of 16, so alignment holds.
    subq $32, %rsp

    # Jump into your C kernel main function
    call kernel_main

    # If kernel_main ever returns, halt safely
    cli
.L_halt:
    hlt
    jmp .L_halt
