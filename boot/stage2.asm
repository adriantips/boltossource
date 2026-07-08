; ============================================================================
;  BoltOS  -  Stage 2 bootloader  (loaded at 0x8000 by Stage 1)
;  Real mode -> long mode. Steps:
;    * enable A20
;    * VBE: find & set a 32-bpp linear-framebuffer video mode
;    * BIOS E820 memory map
;    * unreal mode -> copy kernel to physical 1 MiB
;    * build PML4/PDPT/PD (identity 0..4 GiB, 2 MiB pages)
;    * enter 64-bit long mode, jump into kernel with RDI = bootinfo
;
;  KERNEL_SECTORS passed by build.sh (-D) = ceil(kernel_size / 512).
; ============================================================================
[BITS 16]
[ORG 0x8000]

%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 512
%endif

KERNEL_LBA        equ 33
KERNEL_PHYS       equ 0x100000
BOUNCE_SEG        equ 0x7000
BOUNCE_LIN        equ 0x70000
SECTORS_PER_CHUNK equ 64
BOOTINFO          equ 0x0500
E820_BUF          equ 0x0600

PML4_ADDR         equ 0x10000
PDPT_ADDR         equ 0x11000
PD0_ADDR          equ 0x12000
; PD0..PD3 occupy 0x12000..0x15FFF (identity 0..4 GiB, 2 MiB pages).
PDPT_K_ADDR       equ 0x16000        ; PDPT for higher-half kernel (-2 GiB)
PDPT_D_ADDR       equ 0x17000        ; PDPT for direct physical map

; scratch buffers for VBE (real-mode segment 0x7000)
VBE_SEG           equ 0x7000
VBE_INFO_OFF      equ 0x0000          ; VbeInfoBlock  (512 bytes)
MODE_INFO_OFF     equ 0x0200          ; ModeInfoBlock (256 bytes)

stage2_start:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7000
    sti
    mov     [boot_drive], dl

    mov     si, msg_stage2
    call    rm_print

    in      al, 0x92                 ; enable A20 (fast gate)
    or      al, 2
    out     0x92, al

    call    do_vbe                   ; pick + set linear framebuffer
    call    do_e820                  ; memory map
    call    enter_unreal             ; flat FS/GS for >1MiB writes
    call    load_kernel              ; disk -> 1 MiB

    mov     si, msg_loaded
    call    rm_print

    call    build_paging

    mov     dword [BOOTINFO + 28], E820_BUF     ; bootinfo.e820_addr

    ; ================= enter long mode =================
    cli
    lgdt    [gdt64.ptr]
    mov     eax, cr4
    or      eax, 1 << 5             ; PAE
    mov     cr4, eax
    mov     eax, PML4_ADDR
    mov     cr3, eax
    mov     ecx, 0xC0000080         ; EFER
    rdmsr
    or      eax, 1 << 8             ; LME
    wrmsr
    mov     eax, cr0
    or      eax, (1 << 31) | 1      ; PG | PE
    mov     cr0, eax
    jmp     0x08:long_mode_entry

; ----------------------------------------------------------------------------
; 16-bit helpers
; ----------------------------------------------------------------------------
rm_print:
.l: lodsb
    test    al, al
    jz      .d
    mov     ah, 0x0E
    mov     bx, 7
    int     0x10
    jmp     .l
.d: ret

; ---- VBE: find first 32-bpp LFB graphics mode (prefer 1024x768) and set it ----
do_vbe:
    ; default = no framebuffer
    mov     dword [BOOTINFO + 0],  0
    mov     dword [BOOTINFO + 4],  0
    mov     dword [BOOTINFO + 8],  0
    mov     dword [BOOTINFO + 12], 0
    mov     dword [BOOTINFO + 16], 0
    mov     dword [BOOTINFO + 20], 0
    mov     word  [pref_mode], 0xFFFF

    mov     ax, VBE_SEG
    mov     es, ax
    mov     di, VBE_INFO_OFF
    mov     dword [es:di], 0x32454256        ; 'VBE2'
    mov     ax, 0x4F00
    int     0x10
    cmp     ax, 0x004F
    jne     .done

    mov     ax, [es:VBE_INFO_OFF + 0x10]     ; VideoModePtr segment
    mov     [mode_seg], ax
    mov     ax, [es:VBE_INFO_OFF + 0x0E]     ; VideoModePtr offset
    mov     [mode_off], ax

.next:
    mov     fs, [mode_seg]
    mov     si, [mode_off]
    mov     cx, [fs:si]
    cmp     cx, 0xFFFF
    je      .choose
    add     word [mode_off], 2

    mov     ax, VBE_SEG
    mov     es, ax
    mov     di, MODE_INFO_OFF
    mov     ax, 0x4F01                       ; CX = mode
    int     0x10
    cmp     ax, 0x004F
    jne     .next

    mov     ax, [es:MODE_INFO_OFF + 0x00]    ; mode attributes
    and     ax, 0x90                         ; bit7 LFB + bit4 graphics
    cmp     ax, 0x90
    jne     .next
    mov     al, [es:MODE_INFO_OFF + 0x19]    ; bits per pixel
    cmp     al, 32
    jne     .next

    cmp     word [pref_mode], 0xFFFF         ; remember first candidate
    jne     .checkres
    mov     [pref_mode], cx
.checkres:
    mov     ax, [es:MODE_INFO_OFF + 0x12]    ; XResolution
    cmp     ax, 1024
    jne     .next
    mov     ax, [es:MODE_INFO_OFF + 0x14]    ; YResolution
    cmp     ax, 768
    jne     .next
    mov     [pref_mode], cx                  ; exact 1024x768 -> take it
    jmp     .choose

.choose:
    mov     cx, [pref_mode]
    cmp     cx, 0xFFFF
    je      .done

    mov     ax, VBE_SEG                      ; refresh ModeInfo for chosen mode
    mov     es, ax
    mov     di, MODE_INFO_OFF
    mov     ax, 0x4F01
    int     0x10
    cmp     ax, 0x004F
    jne     .done

    mov     bx, [pref_mode]                 ; set mode with LFB bit
    or      bx, 0x4000
    mov     ax, 0x4F02
    int     0x10
    cmp     ax, 0x004F
    jne     .done

    mov     ax, VBE_SEG
    mov     es, ax
    mov     eax, [es:MODE_INFO_OFF + 0x28]   ; PhysBasePtr
    mov     [BOOTINFO + 0], eax
    movzx   eax, word [es:MODE_INFO_OFF + 0x12]
    mov     [BOOTINFO + 8], eax              ; width
    movzx   eax, word [es:MODE_INFO_OFF + 0x14]
    mov     [BOOTINFO + 12], eax             ; height
    movzx   eax, word [es:MODE_INFO_OFF + 0x10]
    mov     [BOOTINFO + 16], eax             ; pitch (BytesPerScanLine)
    movzx   eax, byte [es:MODE_INFO_OFF + 0x19]
    mov     [BOOTINFO + 20], eax             ; bpp
.done:
    xor     ax, ax
    mov     es, ax
    ret

; ---- E820 memory map -> E820_BUF, count -> BOOTINFO+24 ----
do_e820:
    xor     ax, ax
    mov     es, ax
    mov     di, E820_BUF
    xor     ebx, ebx
    xor     bp, bp
    mov     edx, 0x534D4150
    mov     eax, 0xE820
    mov     dword [es:di + 20], 1
    mov     ecx, 24
    int     0x15
    jc      .done
    cmp     eax, 0x534D4150
    jne     .done
    jmp     .check
.nexte:
    mov     eax, 0xE820
    mov     dword [es:di + 20], 1
    mov     ecx, 24
    int     0x15
    jc      .done
.check:
    jcxz    .skip
    inc     bp
    add     di, 24
.skip:
    test    ebx, ebx
    jz      .done
    jmp     .nexte
.done:
    movzx   eax, bp
    mov     [BOOTINFO + 24], eax
    ret

; ---- unreal mode: cache flat 4 GiB descriptor in FS and GS ----
enter_unreal:
    cli
    push    eax
    lgdt    [gdt64.ptr]
    mov     eax, cr0
    or      al, 1
    mov     cr0, eax
    jmp     .pflush
.pflush:
    mov     bx, 0x10
    mov     fs, bx
    mov     gs, bx
    mov     eax, cr0
    and     al, 0xFE
    mov     cr0, eax
    jmp     .rflush
.rflush:
    pop     eax
    sti
    ret

; ---- load kernel: disk -> bounce buffer -> 1 MiB (flat FS/GS) ----
load_kernel:
    mov     dword [cur_lba],   KERNEL_LBA
    mov     dword [dest_off],  KERNEL_PHYS
    mov     dword [remaining], KERNEL_SECTORS
.loop:
    mov     eax, [remaining]
    test    eax, eax
    jz      .done
    mov     ecx, SECTORS_PER_CHUNK
    cmp     eax, ecx
    jae     .have
    mov     ecx, eax
.have:
    mov     [dap_count], cx
    mov     word [dap_off], 0
    mov     word [dap_seg], BOUNCE_SEG
    mov     eax, [cur_lba]
    mov     [dap_lba], eax
    mov     dword [dap_lba + 4], 0
    mov     si, dap
    mov     ah, 0x42
    mov     dl, [boot_drive]
    int     0x13
    jc      .err
    mov     esi, BOUNCE_LIN
    mov     edi, [dest_off]
    movzx   ecx, word [dap_count]
    shl     ecx, 7
.copy:
    mov     eax, [gs:esi]
    mov     [fs:edi], eax
    add     esi, 4
    add     edi, 4
    dec     ecx
    jnz     .copy
    movzx   eax, word [dap_count]
    add     [cur_lba], eax
    movzx   eax, word [dap_count]
    shl     eax, 9
    add     [dest_off], eax
    movzx   eax, word [dap_count]
    sub     [remaining], eax
    jmp     .loop
.done:
    ret
.err:
    mov     si, msg_diskerr
    call    rm_print
    cli
    hlt
    jmp     .err

; ---- page tables --------------------------------------------------------
;   PML4[0]   -> identity map      0..4 GiB           (boot / phys access)
;   PML4[256] -> direct phys map   0xFFFF800000000000 -> phys 0..4 GiB
;   PML4[511] -> higher-half kernel 0xFFFFFFFF80000000 -> phys 0..1 GiB
;  All share the same four 2 MiB-page PDs (PD0..PD3).
build_paging:
    mov     ax, 0x1000
    mov     es, ax
    xor     di, di
    xor     ax, ax
    mov     cx, 0x4000               ; zero 0x10000..0x18000 (incl. PDPT_K/PDPT_D)
    rep     stosw

    ; PML4 entries
    mov     dword [es:0x0000], PDPT_ADDR   | 3        ; [0]   identity
    mov     dword [es:0x0800], PDPT_D_ADDR | 3        ; [256] direct map
    mov     dword [es:0x0FF8], PDPT_K_ADDR | 3        ; [511] higher-half kernel

    ; identity PDPT[0..3] -> PD0..PD3
    mov     dword [es:0x1000], (PD0_ADDR + 0x0000) | 3
    mov     dword [es:0x1008], (PD0_ADDR + 0x1000) | 3
    mov     dword [es:0x1010], (PD0_ADDR + 0x2000) | 3
    mov     dword [es:0x1018], (PD0_ADDR + 0x3000) | 3

    ; direct-map PDPT[0..3] -> PD0..PD3 (PDPT_D @ es:0x7000)
    mov     dword [es:0x7000], (PD0_ADDR + 0x0000) | 3
    mov     dword [es:0x7008], (PD0_ADDR + 0x1000) | 3
    mov     dword [es:0x7010], (PD0_ADDR + 0x2000) | 3
    mov     dword [es:0x7018], (PD0_ADDR + 0x3000) | 3

    ; higher-half PDPT[510] -> PD0  (PDPT_K @ es:0x6000; 0xFFFFFFFF80000000)
    mov     dword [es:0x6FF0], (PD0_ADDR + 0x0000) | 3

    ; fill PD0..PD3: 2048 entries, 2 MiB pages, phys 0..4 GiB
    mov     di, 0x2000
    xor     eax, eax
    mov     cx, 2048
.fill:
    mov     ebx, eax
    or      ebx, 0x83
    mov     [es:di], ebx
    mov     dword [es:di + 4], 0
    add     eax, 0x200000
    add     di, 8
    dec     cx
    jnz     .fill
    xor     ax, ax
    mov     es, ax
    ret

; ----------------------------------------------------------------------------
; 64-bit entry
; ----------------------------------------------------------------------------
[BITS 64]
long_mode_entry:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    mov     rsp, 0x90000
    mov     rdi, BOOTINFO
    mov     rax, KERNEL_PHYS
    jmp     rax

; ----------------------------------------------------------------------------
; data
; ----------------------------------------------------------------------------
[BITS 16]
boot_drive: db 0
cur_lba:    dd 0
dest_off:   dd 0
remaining:  dd 0
mode_seg:   dw 0
mode_off:   dw 0
pref_mode:  dw 0xFFFF

align 4
dap:
    db  0x10
    db  0
dap_count: dw 0
dap_off:   dw 0
dap_seg:   dw 0
dap_lba:   dq 0

align 16
gdt64:
    dq  0x0000000000000000
    dq  0x00AF9A000000FFFF          ; 0x08 code64
    dq  0x00CF92000000FFFF          ; 0x10 data (flat)
.ptr:
    dw  .ptr - gdt64 - 1
    dd  gdt64

msg_stage2:  db "BoltOS S2: A20 ok, VBE + load kernel...", 13, 10, 0
msg_loaded:  db "BoltOS S2: kernel at 1MiB, going long mode.", 13, 10, 0
msg_diskerr: db "BoltOS S2 disk error", 13, 10, 0
