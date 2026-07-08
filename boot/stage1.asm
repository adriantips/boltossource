; ============================================================================
;  BoltOS  -  Stage 1 bootloader  (MBR, 512 bytes, loaded by BIOS at 0x7C00)
;  Job: set up real-mode segments, load Stage 2 from disk, jump to it.
;  No GRUB, no Multiboot. Pure from scratch.
; ============================================================================
[BITS 16]
[ORG 0x7C00]

STAGE2_LBA      equ 1            ; Stage 2 starts at LBA 1 (right after MBR)
STAGE2_SECTORS  equ 32          ; load 32 sectors (16 KiB) for Stage 2
STAGE2_SEG      equ 0x0000
STAGE2_OFF      equ 0x8000      ; load Stage 2 to linear 0x8000

start:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00          ; stack grows down from just below the MBR
    sti

    mov     [boot_drive], dl    ; BIOS leaves the boot drive number in DL

    ; --- load Stage 2 via INT 13h extended read (LBA / DAP) ---
    mov     si, dap
    mov     ah, 0x42
    mov     dl, [boot_drive]
    int     0x13
    jc      disk_error

    mov     dl, [boot_drive]    ; pass boot drive on to Stage 2
    jmp     STAGE2_SEG:STAGE2_OFF

disk_error:
    mov     si, err_msg
.print:
    lodsb
    test    al, al
    jz      .hang
    mov     ah, 0x0E
    mov     bx, 0x0007
    int     0x10
    jmp     .print
.hang:
    cli
    hlt
    jmp     .hang

boot_drive: db 0

align 4
dap:                            ; Disk Address Packet for INT 13h / AH=42h
    db  0x10                    ; size of packet
    db  0                       ; reserved
    dw  STAGE2_SECTORS          ; number of sectors to transfer
    dw  STAGE2_OFF              ; destination offset
    dw  STAGE2_SEG              ; destination segment
    dq  STAGE2_LBA              ; starting LBA

err_msg: db "BoltOS S1 disk error", 0

times 510 - ($ - $$) db 0
dw 0xAA55
