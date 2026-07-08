; ============================================================================
;  BoltOS  -  ring-3 user program (position-independent blob).
;  Lives in the kernel image as data; the kernel copies these bytes into a
;  USER-mapped page and enters ring 3 at the copy. Only RIP-relative refs,
;  immediates, relative jumps and `syscall` are used, so it runs unchanged at
;  whatever virtual address it is mapped to. Loops: write "U\n", yield, delay.
; ============================================================================
[BITS 64]

section .rodata
global user_blob_start
global user_blob_end

user_blob_start:
.loop:
    mov     rax, 1                  ; SYS_WRITE
    lea     rdi, [rel .msg]         ; ptr (RIP-relative -> self-adjusts)
    mov     rsi, 2                  ; len
    syscall

    mov     rax, 2                  ; SYS_YIELD
    syscall

    mov     rcx, 0x4000000          ; throttle so serial isn't flooded
.delay:
    dec     rcx
    jnz     .delay

    jmp     .loop
.msg:
    db      "U", 10
user_blob_end:
