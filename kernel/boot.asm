; ============================================================================
;  BoltOS  -  64-bit kernel entry stub (.text.boot, linked first at 0x100000)
;  Stage 2 jumps here already in long mode with RDI = bootinfo pointer.
;  Sets up the kernel stack, zeroes BSS, then calls kmain(bootinfo).
; ============================================================================
[BITS 64]

global _start
extern kmain
extern __bss_start
extern __bss_end

; struct bootinfo (include/boot.h), and the E820 table it points at. The
; offsets are already spelled out in stage2.asm; kernel/main.c carries static
; assertions so a change to the struct breaks the build rather than the boot.
KERNEL_VBASE    equ 0xFFFFFFFF80000000
BOOTINFO_SZ     equ 40
BI_E820_COUNT   equ 24
BI_E820_ADDR    equ 28
E820_ENT        equ 24
E820_MAX        equ 256                 ; what uefi_boot.c will emit at most

section .text.boot
_start:
    cli
    ; Entered at the physical load address (identity-mapped). Stage 2 already
    ; installed the higher-half kernel mapping, so jump to our linked virtual
    ; address and run from the higher half from here on.
    mov     rax, .high
    jmp     rax
.high:
    lea     rsp, [rel stack_top]    ; real kernel stack (higher-half)
    mov     rbx, rdi                ; preserve bootinfo pointer (phys, identity-mapped)

    ; --- enable hardware FPU/SSE before any SSE-emitting C runs ---
    ; CR0: clear EM (bit2, no x87 emulation), set MP (bit1, monitor coproc)
    mov     rax, cr0
    and     ax, 0xFFFB              ; ~EM
    or      ax, 0x0002             ; MP
    mov     cr0, rax
    ; CR4: set OSFXSR (bit9, enable FXSAVE/SSE) + OSXMMEXCPT (bit10, #XM faults)
    mov     rax, cr4
    or      rax, (1 << 9) | (1 << 10)
    mov     cr4, rax

    ; breadcrumb: green bar means the kernel's own entry stub is running, so the
    ; long-mode switch and the higher-half mapping both took.
    cld
    mov     r10d, [rbx + 0]                 ; bootinfo.fb_addr
    test    r10d, r10d
    jz      .no_mark_entry
    mov     r8d, [rbx + 16]                 ; bootinfo.fb_pitch
    xor     r9d, r9d
.mark_entry:
    mov     eax, r9d
    imul    eax, r8d
    mov     rdi, r10
    add     rdi, rax
    add     rdi, 170 * 4
    mov     ecx, 160
    mov     eax, 0x0000FF00
    rep     stosd
    inc     r9d
    cmp     r9d, 32
    jb      .mark_entry
.no_mark_entry:

    ; --- rescue bootinfo before the .bss wipe --------------------------------
    ; The UEFI loader takes its bootinfo pages from the firmware with
    ; AllocateAnyPages, so they can land anywhere in RAM -- including inside
    ; this kernel's own .bss, which the loop below is about to fill with zeroes,
    ; taking the memory map with it. That is not hypothetical: growing the
    ; kernel by twenty kilobytes moved .bss over the page the firmware had
    ; picked, and the machine booted to "0 MiB free" with no ACPI and no disks.
    ;
    ; So copy the struct and its E820 table into .data, which nothing zeroes,
    ; and hand kmain the copy. The legacy loader keeps bootinfo at 0x0500 in low
    ; memory and never had the problem; doing this for both is one code path
    ; instead of two, and costs a few microseconds once.
    lea     rdi, [rel bootinfo_copy]
    mov     rsi, rbx
    mov     ecx, BOOTINFO_SZ
    rep     movsb

    lea     rbx, [rel bootinfo_copy]        ; the copy is bootinfo from here on
    mov     ecx, [rbx + BI_E820_COUNT]
    cmp     ecx, E820_MAX
    jbe     .e820_fits
    mov     ecx, E820_MAX                   ; a garbled count must not run away
    mov     [rbx + BI_E820_COUNT], ecx
.e820_fits:
    imul    ecx, ecx, E820_ENT
    mov     esi, [rbx + BI_E820_ADDR]       ; physical; the loader identity-maps it
    lea     rdi, [rel e820_copy]
    rep     movsb
    lea     rax, [rel e820_copy]
    mov     r8, KERNEL_VBASE
    sub     rax, r8                         ; kmain reads it back through P2V()
    mov     [rbx + BI_E820_ADDR], eax

    ; zero .bss
    lea     rdi, [rel __bss_start]
    lea     rcx, [rel __bss_end]
    sub     rcx, rdi
    xor     eax, eax
    rep     stosb

    ; breadcrumb: blue bar means bootinfo survived the rescue and .bss is clear,
    ; so anything still missing after this is kmain's own doing.
    cld
    mov     r10d, [rbx + 0]
    test    r10d, r10d
    jz      .no_mark_kmain
    mov     r8d, [rbx + 16]
    xor     r9d, r9d
.mark_kmain:
    mov     eax, r9d
    imul    eax, r8d
    mov     rdi, r10
    add     rdi, rax
    add     rdi, 340 * 4
    mov     ecx, 160
    mov     eax, 0x000000FF
    rep     stosd
    inc     r9d
    cmp     r9d, 32
    jb      .mark_kmain
.no_mark_kmain:

    mov     rdi, rbx                ; arg0 = bootinfo
    xor     rbp, rbp
    call    kmain

.hang:
    cli
    hlt
    jmp     .hang

; Not .bss: the whole point is that the wipe above does not reach these.
section .data
align 16
bootinfo_copy:  times BOOTINFO_SZ            db 0
align 16
e820_copy:      times (E820_MAX * E820_ENT)  db 0

section .bss
align 16
stack_bottom:
    resb 524288                     ; 512 KiB kernel stack (DOOM recursion + compositor)
stack_top:
