; ============================================================================
;  BoltOS  -  Stage 2 bootloader  (loaded at 0x8000 by Stage 1)
;  Real mode -> long mode. Steps:
;    * enable A20
;    * boot menu (normal / safe mode / recovery), 5 s timeout
;    * read the BoltOS boot header (LBA 33) for the kernel's location + size
;    * VBE: find & set a 32-bpp linear-framebuffer video mode
;    * BIOS E820 memory map
;    * unreal mode -> copy kernel to physical 1 MiB
;    * build PML4/PDPT/PD (identity 0..4 GiB, 2 MiB pages)
;    * enter 64-bit long mode, jump into kernel with RDI = bootinfo
;
;  The kernel's size is NOT baked in here any more: it is read at run time from
;  the boot header at LBA 33 (see include/boot.h). That decoupling is what lets
;  the kernel carry a prebuilt copy of this stage2 as a blob, which is what the
;  `install` command writes onto a target disk. KERNEL_SECTORS (-D from
;  build.sh) survives only as the fallback for a disk with no header.
; ============================================================================
[BITS 16]
[ORG 0x8000]

%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 32768
%endif

BOOT_HDR_LBA      equ 33            ; one-sector BoltOS boot header
KERNEL_LBA        equ 34            ; default kernel start (header overrides)
HDR_BUF           equ 0x2600        ; scratch for the header sector
                                    ; (clear of E820_BUF's upward growth)
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

; Written into both ends of the bounce buffer before every read. A BIOS
; that reports success without moving a byte is a real thing on USB-HDD
; emulation, and it is indistinguishable from a good read unless you look.
POISON            equ 0x5A5AA5A5
MAX_READ_TRIES    equ 4

stage2_start:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7000
    sti
    mov     [boot_drive], dl

    call    ser_init
    mov     si, msg_stage2
    call    s2_print

    call    enable_a20               ; BIOS, then fast gate, then i8042 -- verified
    jnc     .a20_done
    mov     si, msg_a20fail
    call    s2_print
    cli
    hlt
    jmp     $
.a20_done:

    call    boot_menu                ; normal / safe / recovery -> [boot_flags]
    call    read_bhdr                ; kernel LBA + sector count from LBA 33
    call    grab_font                ; BIOS glyphs, while there is a BIOS
    call    do_vbe                   ; pick + set linear framebuffer
    call    do_e820                  ; memory map
    call    enter_unreal             ; flat FS/GS for >1MiB writes
    call    load_kernel              ; disk -> 1 MiB

    call    recheck_a20              ; before anything trusts 1 MiB again

    mov     si, msg_loaded
    call    s2_print

    call    verify_kernel            ; did the bytes actually arrive?
    jnc     .kernel_ok
    mov     si, msg_retry
    call    s2_print
    mov     word [force_chunk], 1
    call    load_kernel
    call    recheck_a20
    call    verify_kernel
    jc      kernel_bad
.kernel_ok:

    call    build_paging

    mov     si, msg_paged
    call    s2_print

    mov     dword [BOOTINFO + 28], E820_BUF     ; bootinfo.e820_addr
    mov     eax, [boot_flags]
    mov     [BOOTINFO + 32], eax                ; bootinfo.boot_flags
    movzx   eax, byte [boot_drive]
    mov     [BOOTINFO + 36], eax                ; bootinfo.boot_drive

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

; ---- console ---------------------------------------------------------------
; Print DS:SI wherever it can actually be seen. While the machine is still in
; text mode that is the BIOS teletype; once a VBE mode is set it is the linear
; framebuffer, because int 10h output after a mode set is a coin toss -- some
; firmware keeps rendering its console into the same buffer, some drops it on
; the floor. Writing to both would have the two fight over the same pixels, so
; this picks one. Everything stage2 has to say after the mode set goes through
; here, which is the difference between a diagnosable failure and a coloured
; rectangle.
s2_print:
    call    ser_print
    cmp     byte [fb_ready], 0
    jne     fb_print
    jmp     rm_print

; ---- COM1 ------------------------------------------------------------------
; 115200 8N1, written straight to the UART rather than through INT 14h, which
; is slow and which several BIOSes implement badly or not at all. On a machine
; with no UART at all the out instructions go nowhere and the LSR reads back
; 0xFF, so the spin below has a bound on it -- an unbounded wait for a THR
; that will never empty would hang the boot on exactly the headless machines
; this is meant to help.
ser_init:
    push    ax
    push    dx
    mov     dx, 0x3F9
    xor     al, al
    out     dx, al                   ; no interrupts, we poll
    mov     dx, 0x3FB
    mov     al, 0x80
    out     dx, al                   ; DLAB on
    mov     dx, 0x3F8
    mov     al, 1
    out     dx, al                   ; divisor 1 -> 115200
    mov     dx, 0x3F9
    xor     al, al
    out     dx, al
    mov     dx, 0x3FB
    mov     al, 0x03
    out     dx, al                   ; 8N1, DLAB off
    mov     dx, 0x3FA
    mov     al, 0xC7
    out     dx, al                   ; FIFOs on, cleared
    mov     dx, 0x3FC
    mov     al, 0x0B
    out     dx, al                   ; DTR + RTS + OUT2

    ; Is there a UART here at all? The scratch register is the usual probe:
    ; write a pattern, read it back, and an absent chip floats every read to
    ; 0xFF. A 486-era 8250 has no scratch register and will be missed, which
    ; costs that machine its boot log and nothing else.
    mov     dx, 0x3FF
    mov     al, 0xA5
    out     dx, al
    in      al, dx
    cmp     al, 0xA5
    sete    al
    mov     [ser_ok], al
    pop     dx
    pop     ax
    ret

ser_putc:
    cmp     byte [ser_ok], 0
    je      .gone
    push    ax
    push    cx
    push    dx
    mov     ah, al
    xor     cx, cx                   ; 65535 polls, then give up on it
.wait:
    mov     dx, 0x3FD
    in      al, dx
    test    al, 0x20                 ; transmitter holding register empty
    jnz     .send
    dec     cx
    jnz     .wait
    jmp     .out
.send:
    mov     dx, 0x3F8
    mov     al, ah
    out     dx, al
.out:
    pop     dx
    pop     cx
    pop     ax
.gone:
    ret

ser_print:
    cmp     byte [ser_ok], 0
    je      .gone
    push    si
    push    ax
.l: lodsb
    test    al, al
    jz      .d
    call    ser_putc
    jmp     .l
.d: pop     ax
    pop     si
.gone:
    ret

; Ask the BIOS for its own 8x8 glyphs while there is still a BIOS to ask.
; INT 10h AX=1130h/BH=03h hands back ES:BP -> the double-dot table, 8 bytes
; per character. Flattening it to a linear address here is what lets the
; framebuffer console below carry no font of its own.
grab_font:
    push    es
    push    bp
    xor     ax, ax
    mov     es, ax
    xor     bp, bp
    mov     ax, 0x1130
    mov     bh, 0x03
    int     0x10
    xor     eax, eax
    mov     ax, es
    shl     eax, 4
    movzx   ebx, bp
    add     eax, ebx
    mov     [font_lin], eax
    pop     bp
    pop     es
    ret

fb_print:
    pushad
    call    enter_unreal             ; BIOS calls are free to drop the limits
.n: lodsb
    test    al, al
    jz      .d
    call    fb_putc
    jmp     .n
.d: popad
    ret

; One 8x8 glyph, white on the same slate the screen was cleared to. Assumes
; the 32-bpp mode do_vbe insists on, so a pixel is one dword and a character
; cell is 32 bytes wide.
fb_putc:
    pushad
    cmp     al, 10
    je      .nl
    cmp     al, 13
    je      .cr
    cmp     al, 32
    jb      .out                     ; ignore the other control bytes

    movzx   ebx, al                  ; glyph = font_lin + char*8
    shl     ebx, 3
    add     ebx, [font_lin]

    mov     eax, [fb_pitch]          ; pixel = fb + cy*8*pitch + cx*8*4
    movzx   ecx, word [fb_cy]
    shl     ecx, 3
    mul     ecx
    mov     edi, eax
    movzx   eax, word [fb_cx]
    shl     eax, 5
    add     edi, eax
    add     edi, [fb_base]

    mov     dx, 8                    ; eight rows
.row:
    mov     al, [fs:ebx]
    mov     ah, al
    push    edi
    mov     cx, 8                    ; eight columns, MSB first
.col:
    test    ah, 0x80
    jz      .off
    mov     dword [fs:edi], 0x00FFFFFF
    jmp     .adv
.off:
    mov     dword [fs:edi], 0x00202840
.adv:
    add     edi, 4
    shl     ah, 1
    dec     cx
    jnz     .col
    pop     edi
    add     edi, [fb_pitch]
    inc     ebx
    dec     dx
    jnz     .row

    inc     word [fb_cx]
    mov     ax, [fb_cols]
    cmp     ax, [fb_cx]
    ja      .out
.nl:
    mov     word [fb_cx], 0
    inc     word [fb_cy]
    mov     ax, [fb_rows]
    cmp     ax, [fb_cy]
    ja      .out
    mov     word [fb_cy], 0          ; wrap rather than scroll; stage2 is brief
    jmp     .out
.cr:
    mov     word [fb_cx], 0
.out:
    popad
    ret

; Paint the whole visible area, so the firmware's leftover console is not
; showing underneath our own text. Done with a plain store loop rather than
; rep stosd because that would need ES flat, and every other routine in here
; reloads ES as a real-mode segment whenever it feels like it.
fb_clear:
    pushad
    call    enter_unreal
    movzx   ecx, word [fb_rows]
    shl     ecx, 3                   ; scanlines to paint
    mov     esi, [fb_base]           ; start of the current scanline
.line:
    mov     edi, esi
    movzx   ebx, word [fb_cols]
    shl     ebx, 3                   ; columns -> pixels
.px:
    mov     dword [fs:edi], 0x00202840
    add     edi, 4
    dec     ebx
    jnz     .px
    add     esi, [fb_pitch]
    dec     ecx
    jnz     .line
    popad
    ret

; EAX -> eight hex digits, through whichever console is live.
print_hex32:
    pushad
    mov     di, hexbuf
    mov     cx, 8
.d: rol     eax, 4
    mov     bl, al
    and     bl, 0x0F
    add     bl, '0'
    cmp     bl, '9'
    jbe     .p
    add     bl, 7
.p: mov     [di], bl
    inc     di
    dec     cx
    jnz     .d
    mov     byte [di], 0
    mov     si, hexbuf
    call    s2_print
    popad
    ret

; ---- A20 -------------------------------------------------------------------
; QEMU always honours the port-0x92 "fast gate", but real chipsets don't all
; agree, and a dead A20 line wraps the kernel copy at 1 MiB back to address 0,
; corrupting the IVT and turning "jmp KERNEL_PHYS" into a jump into garbage --
; the exact hang this guards against. Try the BIOS call, then the fast gate,
; then the i8042 keyboard-controller method, testing after each; only give up
; (CF=1) once all three have failed to actually flip the line.
enable_a20:
    call    test_a20
    jnc     .done

    mov     ax, 0x2401              ; BIOS: enable A20
    int     0x15
    call    test_a20
    jnc     .done

    in      al, 0x92                ; fast gate
    or      al, 2
    and     al, 0xFE                ; don't touch bit 0 (fast reset)
    out     0x92, al
    call    test_a20
    jnc     .done

    call    a20_kbc                 ; i8042 keyboard-controller method
    call    test_a20
.done:
    ret

; CF=0 if A20 is enabled, CF=1 if still wrapping. Writes+restores two bytes
; that straddle the 1 MiB wrap point (0000:0500 vs FFFF:0510) rather than
; trusting whatever happens to already be there.
test_a20:
    push    ds
    push    es
    push    di
    push    si
    push    ax
    push    bx
    cli

    xor     ax, ax
    mov     es, ax
    mov     di, 0x0500
    mov     ax, 0xFFFF
    mov     ds, ax
    mov     si, 0x0510

    mov     al, [es:di]
    mov     bl, [ds:si]
    push    ax
    push    bx

    mov     byte [es:di], 0x00
    mov     byte [ds:si], 0xFF
    cmp     byte [es:di], 0xFF       ; wrapped -> still equals the byte we

    pop     bx
    mov     [ds:si], bl
    pop     ax
    mov     [es:di], al

    jne     .enabled
    stc
    jmp     .out
.enabled:
    clc
.out:
    sti
    pop     bx
    pop     ax
    pop     si
    pop     di
    pop     es
    pop     ds
    ret

a20_kbc:
    cli
    call    .wait_in
    mov     al, 0xAD                ; disable keyboard
    out     0x64, al

    call    .wait_in
    mov     al, 0xD0                ; read controller output port
    out     0x64, al

    call    .wait_out
    in      al, 0x60
    push    ax

    call    .wait_in
    mov     al, 0xD1                ; write controller output port
    out     0x64, al

    call    .wait_in
    pop     ax
    or      al, 2                   ; set A20
    out     0x60, al

    call    .wait_in
    mov     al, 0xAE                ; re-enable keyboard
    out     0x64, al

    call    .wait_in
    sti
    ret
.wait_in:
    in      al, 0x64
    test    al, 2
    jnz     .wait_in
    ret
.wait_out:
    in      al, 0x64
    test    al, 1
    jz      .wait_out
    ret

; ---- boot menu -------------------------------------------------------------
; Draws the entry list and waits BOOT_MENU_SECS for a digit, counting down off
; the BIOS tick counter at 0040:006C (~18.2 Hz). Any other key boots the default
; immediately, as does the timeout. Sets [boot_flags] for the kernel.
BOOT_MENU_SECS  equ 5
TICKS_PER_SEC   equ 18                  ; 18.2 Hz, rounded down (menu is coarse)

boot_menu:
    mov     si, msg_menu
    call    rm_print

    xor     ax, ax
    mov     es, ax
    mov     eax, [es:0x046C]            ; BIOS tick counter
    mov     [menu_t0], eax
    mov     byte [menu_left], BOOT_MENU_SECS
    call    menu_countdown

.poll:
    mov     ah, 0x01                    ; keystroke available?
    int     0x16
    jnz     .key

    xor     ax, ax
    mov     es, ax
    mov     eax, [es:0x046C]
    sub     eax, [menu_t0]
    ; elapsed whole seconds = eax / TICKS_PER_SEC
    xor     edx, edx
    mov     ecx, TICKS_PER_SEC
    div     ecx                         ; eax = seconds elapsed
    cmp     al, BOOT_MENU_SECS
    jae     .done                       ; timed out -> default entry
    mov     ah, BOOT_MENU_SECS
    sub     ah, al                      ; seconds remaining
    cmp     ah, [menu_left]
    je      .poll                       ; same second: keep polling
    mov     [menu_left], ah
    call    menu_countdown
    jmp     .poll

.key:
    mov     ah, 0x00                    ; consume the key
    int     0x16
    cmp     al, '2'
    je      .safe
    cmp     al, '3'
    je      .recovery
    cmp     al, '4'
    je      .fsck
    jmp     .done                       ; 1 / Enter / anything else -> normal

.safe:
    mov     dword [boot_flags], 1       ; BOOT_SAFE
    mov     si, msg_pick_safe
    jmp     .say
.recovery:
    mov     dword [boot_flags], 3       ; BOOT_SAFE | BOOT_RECOVERY
    mov     si, msg_pick_recovery
    jmp     .say
.fsck:
    mov     dword [boot_flags], 4       ; BOOT_FSCK
    mov     si, msg_pick_fsck
.say:
    call    s2_print
    ret
.done:
    mov     si, msg_pick_normal
    call    s2_print
    ret

; Repaint the "booting in N" line in place (CR, no LF).
menu_countdown:
    mov     si, msg_count
    call    rm_print
    mov     al, [menu_left]
    add     al, '0'
    mov     ah, 0x0E
    mov     bx, 7
    int     0x10
    mov     si, msg_count_tail
    call    rm_print
    ret

; ---- boot header (LBA 33) --------------------------------------------------
; Fills [kern_lba] / [kern_sectors] from the on-disk header. A disk written by
; an older build (or a read error) keeps the assembly-time defaults, so this is
; always safe to attempt.
read_bhdr:
    mov     dword [kern_lba],     KERNEL_LBA
    mov     dword [kern_sectors], KERNEL_SECTORS

    mov     word [dap_count], 1
    mov     word [dap_off],   HDR_BUF
    mov     word [dap_seg],   0
    mov     dword [dap_lba],     BOOT_HDR_LBA
    mov     dword [dap_lba + 4], 0
    mov     si, dap
    mov     ah, 0x42
    mov     dl, [boot_drive]
    int     0x13
    jc      .done

    xor     ax, ax
    mov     es, ax
    mov     di, HDR_BUF
    cmp     dword [es:di],     'BOLT'    ; magic[0..3]
    jne     .done
    cmp     dword [es:di + 4], 'BHDR'    ; magic[4..7]
    jne     .done
    mov     eax, [es:di + 8]             ; version
    test    eax, eax
    jz      .done
    cmp     eax, 2                       ; newer than this stage2 knows
    ja      .done
    mov     [hdr_ver], eax
    mov     eax, [es:di + 12]            ; kernel_lba
    test    eax, eax
    jz      .done
    mov     [kern_lba], eax
    mov     eax, [es:di + 16]            ; kernel_sectors
    test    eax, eax
    jz      .done
    mov     [kern_sectors], eax
    ; v2 adds a sum32 of the whole padded kernel image. A v1 header still
    ; boots -- disks written by an older installer exist -- it just cannot
    ; be checked, which is exactly the situation v2 was added to end.
    cmp     dword [hdr_ver], 2
    jb      .nosum
    mov     eax, [es:di + 28]            ; kernel_sum
    mov     [kern_sum], eax
    mov     byte [have_sum], 1
.nosum:
    mov     si, msg_hdr
    call    s2_print
.done:
    ret

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
    jne     .no_vesa

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
    je      .no_mode

    mov     ax, VBE_SEG                      ; refresh ModeInfo for chosen mode
    mov     es, ax
    mov     di, MODE_INFO_OFF
    mov     ax, 0x4F01
    int     0x10
    cmp     ax, 0x004F
    jne     .no_info

    mov     si, msg_vbe_set                  ; last words while text mode still shows
    call    s2_print

    mov     bx, [pref_mode]                 ; set mode with LFB bit
    or      bx, 0x4000
    mov     ax, 0x4F02
    int     0x10
    cmp     ax, 0x004F
    jne     .no_set

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

    ; Arm the framebuffer console with what the mode turned out to be.
    ; Two things have to hold first: a plausible aperture (a real LFB is
    ; always well above the 1 MiB line, and a zero or low PhysBasePtr means
    ; the firmware filled the block in badly), and a font to draw with. If
    ; either is missing the BIOS teletype stays the console -- worse, but
    ; never worse than scribbling megabytes into whatever lives at a bogus
    ; physical address.
    mov     eax, [BOOTINFO + 0]
    cmp     eax, 0x100000
    jb      .done
    cmp     dword [font_lin], 0
    je      .done
    mov     [fb_base], eax
    mov     eax, [BOOTINFO + 16]
    mov     [fb_pitch], eax
    mov     eax, [BOOTINFO + 8]
    shr     eax, 3
    mov     [fb_cols], ax
    mov     eax, [BOOTINFO + 12]
    shr     eax, 3
    mov     [fb_rows], ax
    mov     word [fb_cx], 0
    mov     word [fb_cy], 0
    mov     byte [fb_ready], 1
    call    fb_clear
    mov     si, msg_banner
    call    s2_print
    jmp     .done

; Each dead end says which one it was: on a CSM machine whose GPU has no legacy
; VBE modes the kernel comes up headless and talks only to COM1, which looks
; exactly like a hang unless the bootloader admits what happened.
.no_vesa:
    mov     si, msg_vbe_none
    jmp     .say
.no_mode:
    mov     si, msg_vbe_nomode
    jmp     .say
.no_info:
    mov     si, msg_vbe_noinfo
    jmp     .say
.no_set:
    mov     si, msg_vbe_noset
.say:
    call    s2_print
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
    mov     eax, [kern_lba]
    mov     [cur_lba], eax
    mov     dword [dest_off],  KERNEL_PHYS
    mov     eax, [kern_sectors]
    mov     [remaining], eax
    mov     word [chunk], SECTORS_PER_CHUNK
    cmp     word [force_chunk], 0    ; set after a failed checksum
    je      .sized
    mov     ax, [force_chunk]
    mov     [chunk], ax
.sized:
    mov     dword [dots], 0
.loop:
    mov     eax, [remaining]
    test    eax, eax
    jz      .done
    movzx   ecx, word [chunk]
    cmp     eax, ecx
    jae     .have
    mov     ecx, eax
.have:
    mov     [dap_count], cx
    call    read_chunk
    jc      .shrink

    ; Re-establish the flat limits before every copy. An INT 13h that went
    ; through a CSM thunk has been in and out of protected mode with our
    ; segment registers in its hands, and a BIOS that saves and restores FS
    ; with push/pop reloads the descriptor cache from a real-mode selector on
    ; the way back -- silently undoing unreal mode. Doing this once before the
    ; loop, as this used to, is only safe on firmware that happens to be
    ; polite about it; QEMU's SeaBIOS is, which is why this never showed up in
    ; a test. It costs a few hundred cycles per 32 KiB.
    call    enter_unreal
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
    call    progress
    jmp     .loop
.shrink:
    ; A BIOS that will not serve 64 sectors in one go will often serve 8, and
    ; one that will not serve 8 will usually serve 1. Halve and try again
    ; before declaring the medium unreadable.
    shr     word [chunk], 1
    jnz     .loop
    jmp     .err
.done:
    cmp     byte [dotted], 0         ; close the progress line, if there was one
    je      .quiet
    mov     byte [dotted], 0         ; the retry pass prints its own row of dots
    mov     si, msg_crlf
    call    s2_print
.quiet:
    ret
.err:
    mov     si, msg_readfail
    call    s2_print
    mov     eax, [cur_lba]
    call    print_hex32
    mov     si, msg_readtail
    call    s2_print
    cli
    hlt
    jmp     .err

; ---- one guarded INT 13h extended read into the bounce buffer -------------
; Returns CF=0 only for a read that demonstrably moved data. Three separate
; things can go wrong here and only one of them sets the carry flag:
;
;   * the honest failure -- CF=1, retried after an AH=00h controller reset;
;   * the short read -- the DAP's count field is in/out and the BIOS writes
;     back what it really transferred, so it is rewritten before every try;
;   * the silent lie -- CF=0, count unchanged, nothing transferred. Both ends
;     of the buffer are poisoned first so that one cannot pass for a good
;     read. This is the one that produces a machine that boots to a coloured
;     rectangle: stage2 completes, and 1 MiB holds whatever it held before.
read_chunk:
    mov     cx, [dap_count]
    mov     [want], cx
    mov     byte [tries], MAX_READ_TRIES
.try:
    call    enter_unreal
    mov     edi, BOUNCE_LIN
    mov     dword [fs:edi], POISON
    movzx   eax, word [want]
    shl     eax, 9
    add     edi, eax
    sub     edi, 4
    mov     dword [fs:edi], POISON

    mov     cx, [want]
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
    jc      .retry
    mov     cx, [dap_count]
    test    cx, cx
    jz      .retry

    call    enter_unreal
    mov     edi, BOUNCE_LIN
    cmp     dword [fs:edi], POISON
    jne     .ok
    movzx   eax, word [dap_count]
    shl     eax, 9
    add     edi, eax
    sub     edi, 4
    cmp     dword [fs:edi], POISON
    jne     .ok
.retry:
    dec     byte [tries]
    jz      .fail
    xor     ax, ax                   ; AH=00h: reset the disk controller
    mov     dl, [boot_drive]
    int     0x13
    jmp     .try
.ok:
    clc
    ret
.fail:
    stc
    ret

; A dot per half-megabyte, so a load that is crawling looks different from a
; load that has stopped. On a stick that negotiated USB 1.1 this is the only
; sign the machine is doing anything at all for ten seconds at a time.
progress:
    push    eax
    movzx   eax, word [dap_count]
    add     [dots], eax
    cmp     dword [dots], 1024       ; one dot per half-megabyte
    jb      .skip
    sub     dword [dots], 1024
    mov     byte [dotted], 1
    mov     si, msg_dot
    call    s2_print
.skip:
    pop     eax
    ret

; ---- did the kernel actually arrive? ---------------------------------------
; The check that tells a broken boot apart from a broken kernel. stage2 has
; just pulled several megabytes through firmware it cannot audit; if one
; sector quietly did not make it, the jump below lands in rubble and the
; machine stops with nothing on screen to say why. Summing what reached 1 MiB
; and comparing it against the sum build.sh recorded turns that into a
; sentence. Runs before paging so it can use the flat data segments.
verify_kernel:                       ; CF=1 if what is at 1 MiB is not it
    cmp     byte [have_sum], 0
    je      .ok                      ; v1 header: nothing to compare against
    call    enter_unreal
    mov     esi, KERNEL_PHYS
    mov     ecx, [kern_sectors]
    shl     ecx, 7                   ; sectors -> dwords
    xor     eax, eax
.sum:
    add     eax, [gs:esi]
    add     esi, 4
    dec     ecx
    jnz     .sum
    mov     [kern_got], eax
    cmp     eax, [kern_sum]
    jne     .bad
    mov     si, msg_sumok
    call    s2_print
.ok:
    clc
    ret
.bad:
    stc
    ret

; Both attempts disagreed with the header. Say what was read and what was
; wanted, and stop -- jumping into this is the silent hang the checksum was
; added to replace.
kernel_bad:
    mov     si, msg_sumbad
    call    s2_print
    mov     eax, [kern_got]
    call    print_hex32
    mov     si, msg_sumwant
    call    s2_print
    mov     eax, [kern_sum]
    call    print_hex32
    mov     si, msg_sumhelp
    call    s2_print
    cli
    hlt
    jmp     $

; ---- A20, one more time ----------------------------------------------------
; A20 was proved on before the boot menu, but everything since -- the menu,
; VBE, E820 and several megabytes of disk reads -- went through firmware that
; is entitled to leave the line however it likes, and some CSMs do turn it
; back off on the way out of a thunk. With A20 off every address with bit 20
; set is aliased down, so the kernel that was just written to 1 MiB is really
; sitting at 0, and the jump into it lands in the interrupt vector table.
recheck_a20:
    call    test_a20
    jnc     .ok
    call    enable_a20
    jc      .dead
    mov     si, msg_a20back
    call    s2_print
    ret
.dead:
    mov     si, msg_a20lost
    call    s2_print
    cli
    hlt
    jmp     $
.ok:
    ret

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

    ; Breadcrumbs. BIOS is gone by now, so these are the only way to say how far
    ; we got on a machine with no serial cable. The text cells land in VGA text
    ; memory (seen only if no graphics mode was set); the red bar goes into the
    ; framebuffer itself (seen only if one was). One of the two always shows.
    mov     rax, 0xB8000
    mov     word [rax +  0], 0x4F4C          ; L
    mov     word [rax +  2], 0x4F4D          ; M
    mov     word [rax +  4], 0x4F36          ; 6
    mov     word [rax +  6], 0x4F34          ; 4
    mov     word [rax +  8], 0x4F2D          ; -
    mov     word [rax + 10], 0x4F4F          ; O
    mov     word [rax + 12], 0x4F4B          ; K

    cld
    mov     ebx, [BOOTINFO + 0]              ; fb_addr (phys, identity-mapped)
    test    ebx, ebx
    jz      .no_fb
    mov     r8d, [BOOTINFO + 16]             ; pitch, bytes per scanline
    xor     r9d, r9d
.bar:
    mov     eax, r9d
    imul    eax, r8d
    mov     rdi, rbx
    add     rdi, rax
    mov     ecx, 160
    mov     eax, 0x00FF0000                  ; red: long mode reached
    rep     stosd
    inc     r9d
    cmp     r9d, 32
    jb      .bar
.no_fb:

    mov     rdi, BOOTINFO
    mov     rax, KERNEL_PHYS
    jmp     rax

; ----------------------------------------------------------------------------
; data
; ----------------------------------------------------------------------------
[BITS 16]
boot_drive:   db 0
menu_left:    db 0
menu_t0:      dd 0
boot_flags:   dd 0
kern_lba:     dd 0
kern_sectors: dd 0
cur_lba:      dd 0
dest_off:     dd 0
remaining:    dd 0
mode_seg:     dw 0
mode_off:     dw 0
pref_mode:    dw 0xFFFF
hdr_ver:      dd 0
kern_sum:     dd 0
have_sum:     db 0
chunk:        dw 0
force_chunk:  dw 0
kern_got:     dd 0
want:         dw 0
tries:        db 0
dots:         dd 0
dotted:       db 0

; framebuffer console state, filled in by do_vbe once a mode is really set
font_lin:     dd 0
fb_base:      dd 0
fb_pitch:     dd 0
fb_cols:      dw 0
fb_rows:      dw 0
fb_cx:        dw 0
fb_cy:        dw 0
fb_ready:     db 0
hexbuf:       times 12 db 0
ser_ok:       db 0

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

msg_stage2:  db "BoltOS S2: stage 2 loaded.", 13, 10, 0
msg_a20fail: db "BoltOS S2: A20 line would not enable, stopping here.", 13, 10, 0
msg_paged:   db "BoltOS S2: page tables built.", 13, 10, 0
msg_vbe_set:   db "  VBE: setting the mode now", 13, 10, 0
msg_vbe_none:  db "  VBE: no VESA BIOS here -- kernel will be serial-only", 13, 10, 0
msg_vbe_nomode:db "  VBE: no 32bpp LFB mode offered -- kernel will be serial-only", 13, 10, 0
msg_vbe_noinfo:db "  VBE: mode info refused -- kernel will be serial-only", 13, 10, 0
msg_vbe_noset: db "  VBE: mode set refused -- kernel will be serial-only", 13, 10, 0
msg_loaded:  db "BoltOS S2: kernel at 1MiB, going long mode.", 13, 10, 0
msg_diskerr: db "BoltOS S2 disk error", 13, 10, 0
msg_hdr:     db "  boot header found", 13, 10, 0
msg_banner:  db "BoltOS stage 2", 13, 10, 0
msg_dot:     db ".", 0
msg_crlf:    db 13, 10, 0
msg_sumok:   db 13, 10, "  kernel verified: checksum matches", 13, 10, 0
msg_retry:   db 13, 10, "  checksum mismatch -- reloading one sector at a time", 13, 10, 0
msg_sumbad:  db 13, 10, "BoltOS S2: the kernel did not survive the trip from disk.", 13, 10
             db "  loaded 0x", 0
msg_sumwant: db "  expected 0x", 0
msg_sumhelp: db 13, 10
             db "  Stopping here instead of jumping into it.", 13, 10
             db "  INT 13h reported every one of those reads as a success, so", 13, 10
             db "  this is firmware mis-serving the boot device rather than a", 13, 10
             db "  damaged image. Worth trying, in order: a different USB port", 13, 10
             db "  (a rear one, USB 2.0 if the board has any), then booting the", 13, 10
             db "  stick in UEFI mode instead of CSM.", 13, 10, 0
msg_readfail:db 13, 10, "BoltOS S2: INT 13h will not read LBA 0x", 0
msg_readtail:db 13, 10
             db "  Retried with a controller reset and down to one sector at a", 13, 10
             db "  time, and the drive still refused. The image may be short --", 13, 10
             db "  check that the whole file was written to the stick.", 13, 10, 0
msg_a20back: db "  A20 had gone back off; re-enabled it", 13, 10, 0
msg_a20lost: db 13, 10, "BoltOS S2: A20 went back off and will not re-enable.", 13, 10
             db "  Everything above 1 MiB is aliased; cannot boot.", 13, 10, 0

msg_menu:    db 13, 10
             db "  ==== BoltOS boot menu ====", 13, 10
             db "   1) BoltOS            (normal)", 13, 10
             db "   2) Safe mode         (no desktop, no network)", 13, 10
             db "   3) Recovery shell    (safe + read-only filesystem)", 13, 10
             db "   4) BoltOS + fsck     (check the filesystem first)", 13, 10, 0
msg_count:   db 13, "  starting entry 1 in ", 0
msg_count_tail: db "s ... ", 0
msg_pick_normal:   db 13, 10, "  -> normal boot", 13, 10, 0
msg_pick_safe:     db 13, 10, "  -> SAFE MODE", 13, 10, 0
msg_pick_recovery: db 13, 10, "  -> RECOVERY SHELL", 13, 10, 0
msg_pick_fsck:     db 13, 10, "  -> normal boot + fsck", 13, 10, 0
