; ===========================================================================
;  S3 resume trampoline.
;
;  When the machine wakes from S3 the firmware does not return to where it was.
;  RAM survived; nothing else did. The CPU comes up in 16-bit real mode at the
;  address left in the FACS firmware waking vector, with paging off, long mode
;  off, no GDT worth the name, and the caches in whatever state the platform
;  felt like. Everything from there back to running C is this file.
;
;  Which makes it the AP trampoline with the serial numbers filed off, and that
;  is deliberate: kernel/ap_boot.asm climbs the exact same ladder for a
;  processor woken by a SIPI, it has been debugged, and two subtly different
;  copies of a real->protected->long mode transition is how you end up with a
;  machine that boots but will not wake.
;
;  Copied to physical 0x9000 by kernel/sleep.c. That address is chosen because
;  it is below 1 MiB (the vector is a 20-bit real-mode address), identity
;  mapped, and clear of everything else that wants low memory: the boot sector
;  at 0x7C00, stage 2 and the AP trampoline at 0x8000, and the EBDA up at
;  0x9FC00.
;
;  Parameter block at phys 0x9F00, filled before sleeping:
;     +0x00 cr3        (kernel PML4 physical)
;     +0x08 stack_top  (the resume stack, virtual)
;     +0x10 entry      (s3_resume64, a 64-bit virtual address)
; ===========================================================================
[BITS 16]
ORG 0x9000

%define PARAM 0x9F00

; One byte straight at the serial port at each stage. This is not scaffolding
; to be taken out later: a resume that dies has nothing else to say. There is
; no framebuffer, no kprintf and no stack you can trust yet, and the difference
; between "the firmware never jumped here" and "it jumped here and the climb to
; long mode faulted" is otherwise invisible. Three characters cost nothing and
; answer the only question worth asking.
%macro MARK 1
    mov dx, 0x3F8
    mov al, %1
    out dx, al
%endmacro

wake_start:
    cli
    cld
    ; The firmware's entry CS is not guaranteed, so nothing may be assumed
    ; about the segment registers. DS=0 makes the absolute [label] references
    ; below (which ORG 0x9000 resolved against a zero base) mean what they say.
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    MARK '3'                          ; the firmware jumped here: real mode
    lgdt [gdt32_desc]

    mov eax, cr0
    or  eax, 1                        ; CR0.PE
    mov cr0, eax
    jmp 0x08:pm32

[BITS 32]
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    MARK 'P'                          ; protected mode
    mov eax, cr4
    or  eax, (1 << 5)                 ; PAE
    mov cr4, eax

    mov eax, [PARAM + 0x00]           ; the page tables that survived in RAM
    mov cr3, eax

    mov ecx, 0xC0000080               ; EFER
    rdmsr
    or  eax, (1 << 8) | (1 << 11)     ; LME | NXE
    wrmsr

    mov eax, cr0
    or  eax, (1 << 31) | (1 << 16)    ; PG | WP -> long mode is live
    mov cr0, eax

    jmp 0x18:lm64

[BITS 64]
lm64:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; --- hardware FPU/SSE, before a single line of C runs -----------------
    ; CR4 came back from S3 with everything cleared but the PAE bit this
    ; trampoline just set, and the kernel is compiled -msse -msse2: at -O2 the
    ; compiler vectorises anything that looks like a memset, so the very first
    ; C function to zero a struct executes an SSE store into a CPU where SSE is
    ; off. That is a #UD, and at this point in a resume there is no IDT for it
    ; to land in, so it is a triple fault and a reboot -- which is
    ; indistinguishable from a machine that never woke up.
    ;
    ; Same four instructions as kernel/boot.asm, for exactly the same reason.
    mov rax, cr0
    and ax, 0xFFFB                    ; ~EM  (no x87 emulation)
    or  ax, 0x0002                    ;  MP  (monitor coprocessor)
    mov cr0, rax
    mov rax, cr4
    or  rax, (1 << 9) | (1 << 10)     ; OSFXSR | OSXMMEXCPT
    mov cr4, rax

    MARK 'L'                          ; long mode, page tables live, SSE on
    mov rsp, [PARAM + 0x08]
    mov rax, [PARAM + 0x10]
    jmp rax                           ; into s3_resume64 (never returns here)

.hang:
    hlt
    jmp .hang

; --- self-contained GDT: null, 32-bit code/data, 64-bit code/data ----------
; The kernel's own GDT is a higher-half virtual address and is unreachable
; until paging is on, which is the whole reason this one exists.
align 16
gdt32:
    dq 0x0000000000000000      ; 0x00 null
    dq 0x00CF9A000000FFFF      ; 0x08 32-bit code
    dq 0x00CF92000000FFFF      ; 0x10 32-bit data
    dq 0x00AF9A000000FFFF      ; 0x18 64-bit code (L=1)
    dq 0x00AF92000000FFFF      ; 0x20 64-bit data
gdt32_end:

gdt32_desc:
    dw gdt32_end - gdt32 - 1
    dq gdt32

; pad up to (but not into) the parameter block at 0x9F00
times 0x0F00 - ($ - $$) db 0
