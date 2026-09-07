/* ===========================================================================
 *  BoltOS  -  boot/uefi_boot.c   (BOOTX64.EFI)
 *  A UEFI boot loader so BoltOS boots on modern firmware with no CSM/legacy
 *  BIOS. It reproduces exactly the environment the kernel's _start expects from
 *  the legacy stage2:
 *    - kernel.bin loaded at physical 0x100000 (linked higher-half, LMA 1 MiB)
 *    - long mode with page tables: PML4[0] identity 0..4 GiB, PML4[256] direct
 *      map 0xFFFF800000000000->phys, PML4[511]/PDPT[510] higher-half kernel
 *      0xFFFFFFFF80000000->phys 0..1 GiB
 *    - RDI = pointer to a struct bootinfo {fb_addr,w,h,pitch,bpp,e820_count,
 *      e820_addr} with the GOP framebuffer and an E820 map built from the UEFI
 *      memory map
 *  then jumps to 0x100000. Built with clang --target=x86_64-unknown-windows
 *  (MS ABI, which UEFI uses) and linked as an EFI application.
 * ===========================================================================*/
#include <stdint.h>

typedef uint64_t UINTN;
typedef uint64_t EFI_STATUS;
typedef void    *EFI_HANDLE;
typedef uint16_t CHAR16;

#define EFIAPI __attribute__((ms_abi))
#define EFI_SUCCESS 0
#define EFI_ERR(x)  (0x8000000000000000ull | (x))

typedef struct { uint32_t a; uint16_t b, c; uint8_t d[8]; } EFI_GUID;

/* ---- text output -------------------------------------------------------- */
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *reset;
    EFI_STATUS (EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, CHAR16 *);
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* ---- text input (the boot menu reads keys through this) ------------------ */
typedef struct { uint16_t ScanCode; CHAR16 UnicodeChar; } EFI_INPUT_KEY;
typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, unsigned char);
    /* Returns EFI_NOT_READY when no key is buffered, which is what makes a
     * poll-with-timeout menu possible without an event loop. */
    EFI_STATUS (EFIAPI *ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, EFI_INPUT_KEY *);
    void *WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

/* ---- graphics output ---------------------------------------------------- */
typedef struct {
    uint32_t Version, HorizontalResolution, VerticalResolution, PixelFormat;
    uint32_t RedMask, GreenMask, BlueMask, ReservedMask;
    uint32_t PixelsPerScanLine;
} EFI_GOP_MODE_INFO;
typedef struct {
    uint32_t MaxMode, Mode;
    EFI_GOP_MODE_INFO *Info;
    UINTN SizeOfInfo;
    uint64_t FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GOP_MODE;
typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    void *QueryMode, *SetMode, *Blt;
    EFI_GOP_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ---- simple file system ------------------------------------------------- */
typedef struct EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *Open)(struct EFI_FILE_PROTOCOL *, struct EFI_FILE_PROTOCOL **, CHAR16 *, uint64_t, uint64_t);
    EFI_STATUS (EFIAPI *Close)(struct EFI_FILE_PROTOCOL *);
    void *Delete;
    EFI_STATUS (EFIAPI *Read)(struct EFI_FILE_PROTOCOL *, UINTN *, void *);
    void *Write;
    EFI_STATUS (EFIAPI *GetPosition)(struct EFI_FILE_PROTOCOL *, uint64_t *);
    EFI_STATUS (EFIAPI *SetPosition)(struct EFI_FILE_PROTOCOL *, uint64_t);
    void *GetInfo, *SetInfo, *Flush;
} EFI_FILE_PROTOCOL;
typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *, EFI_FILE_PROTOCOL **);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct {
    uint32_t Revision; EFI_HANDLE ParentHandle; void *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath, *Reserved, *LoadOptions; uint32_t LoadOptionsSize;
    void *ImageBase; uint64_t ImageSize;
} EFI_LOADED_IMAGE_PROTOCOL;

/* ---- boot services ------------------------------------------------------ */
typedef struct {
    uint32_t Type; uint32_t pad;
    uint64_t PhysicalStart, VirtualStart, NumberOfPages, Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct {
    char hdr[24];
    void *RaiseTPL, *RestoreTPL;
    EFI_STATUS (EFIAPI *AllocatePages)(int, int, UINTN, uint64_t *);
    EFI_STATUS (EFIAPI *FreePages)(uint64_t, UINTN);
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *, EFI_MEMORY_DESCRIPTOR *, UINTN *, UINTN *, uint32_t *);
    EFI_STATUS (EFIAPI *AllocatePool)(int, UINTN, void **);
    EFI_STATUS (EFIAPI *FreePool)(void *);
    void *CreateEvent, *SetTimer, *WaitForEvent, *SignalEvent, *CloseEvent, *CheckEvent;
    void *InstallProtocolInterface, *ReinstallProtocolInterface, *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, EFI_GUID *, void **);
    void *Reserved, *RegisterProtocolNotify, *LocateHandle, *LocateDevicePath, *InstallConfigurationTable;
    void *LoadImage, *StartImage, *Exit, *UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE, UINTN);
    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN microseconds);
    void *SetWatchdogTimer;
    void *ConnectController, *DisconnectController;
    void *OpenProtocol, *CloseProtocol, *OpenProtocolInformation;
    void *ProtocolsPerHandle, *LocateHandleBuffer;
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *, void *, void **);
} EFI_BOOT_SERVICES;

typedef struct {
    char hdr[24];
    CHAR16 *FirmwareVendor; uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle; EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE ConsoleOutHandle; EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle; void *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
} EFI_SYSTEM_TABLE;

/* AllocateType / MemoryType */
#define AllocateAnyPages   0
#define AllocateAddress    2
#define EfiLoaderData      2
#define EfiConventionalMemory 7

/* GUIDs */
static EFI_GUID GOP_GUID   = {0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};
static EFI_GUID LIP_GUID   = {0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static EFI_GUID SFS_GUID   = {0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};

/* struct bootinfo -- must match include/boot.h byte for byte */
struct bootinfo {
    uint64_t fb_addr;
    uint32_t fb_width, fb_height, fb_pitch, fb_bpp;
    uint32_t e820_count;
    uint32_t e820_addr;
    uint32_t boot_flags;
    uint32_t boot_drive;
} __attribute__((packed));

/* Mirrors the BOOT_* set in include/boot.h. Kept as literals rather than an
 * include because this file is built with the Windows/MS-ABI target and its own
 * minimal type universe. */
#define BOOT_SAFE      0x00000001u
#define BOOT_RECOVERY  0x00000002u
#define BOOT_FSCK      0x00000004u
#define BOOT_UEFI      0x00000008u

struct e820 { uint64_t base, len; uint32_t type; } __attribute__((packed));

#define KERNEL_PHYS 0x100000ull
#define PAGE 0x1000ull

static EFI_BOOT_SERVICES *BS;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Out;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *In;

static void print(const char *s) {
    CHAR16 buf[128]; int i = 0;
    while (*s && i < 126) { if (*s == '\n') buf[i++] = '\r'; buf[i++] = (CHAR16)*s++; }
    buf[i] = 0;
    Out->OutputString(Out, buf);
}
static void printhex(uint64_t v) {
    CHAR16 buf[20]; const char *h = "0123456789abcdef";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) buf[2 + i] = (CHAR16)h[(v >> ((15 - i) * 4)) & 0xF];
    buf[18] = 0; Out->OutputString(Out, buf);
}

/* ---------------------------------------------------------------------------
 *  Boot menu. The legacy chain grows one in stage2.asm; this is the UEFI twin,
 *  so a machine that will not reach the desktop can be talked down from either
 *  firmware. ReadKeyStroke returns EFI_NOT_READY when nothing is buffered, so
 *  the countdown is just a poll loop around 100 ms Stall()s.
 * ------------------------------------------------------------------------- */
#define MENU_SECONDS 5

static uint32_t boot_menu(void) {
    print("\n  ==== BoltOS boot menu ====\n"
          "   1) BoltOS            (normal)\n"
          "   2) Safe mode         (no desktop, no network)\n"
          "   3) Recovery shell    (safe + read-only filesystem)\n"
          "   4) BoltOS + fsck     (check the filesystem first)\n");
    if (!In) return 0;                       /* no console input: boot default */

    for (int s = MENU_SECONDS; s > 0; s--) {
        char line[48];
        int i = 0;
        line[i++] = '\r'; line[i++] = ' '; line[i++] = ' ';
        const char *t = "starting entry 1 in ";
        while (*t) line[i++] = *t++;
        line[i++] = (char)('0' + s);
        t = "s ... ";
        while (*t) line[i++] = *t++;
        line[i] = 0;
        print(line);

        for (int tenth = 0; tenth < 10; tenth++) {
            EFI_INPUT_KEY k;
            if (In->ReadKeyStroke(In, &k) == EFI_SUCCESS) {
                switch (k.UnicodeChar) {
                case '2': print("\n  -> SAFE MODE\n");      return BOOT_SAFE;
                case '3': print("\n  -> RECOVERY SHELL\n"); return BOOT_SAFE | BOOT_RECOVERY;
                case '4': print("\n  -> normal boot + fsck\n"); return BOOT_FSCK;
                default:  print("\n  -> normal boot\n");    return 0;
                }
            }
            BS->Stall(100000);               /* 100 ms */
        }
    }
    print("\n  -> normal boot\n");
    return 0;
}

static uint64_t alloc_pages(UINTN n) {
    uint64_t addr = 0;
    if (BS->AllocatePages(AllocateAnyPages, EfiLoaderData, n, &addr) != EFI_SUCCESS) return 0;
    return addr;
}
static void zero(void *p, UINTN n) { uint8_t *b = p; for (UINTN i = 0; i < n; i++) b[i] = 0; }

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    BS  = st->BootServices;
    Out = st->ConOut;
    In  = st->ConIn;
    print("BoltOS UEFI loader\n");

    uint32_t bootsel = boot_menu() | BOOT_UEFI;

    /* ---- 1. graphics output: pick the current GOP framebuffer ---- */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    if (BS->LocateProtocol(&GOP_GUID, 0, (void **)&gop) != EFI_SUCCESS || !gop) {
        print("no GOP\n"); for (;;) {}
    }
    EFI_GOP_MODE_INFO *gi = gop->Mode->Info;
    uint64_t fb   = gop->Mode->FrameBufferBase;
    uint32_t fw   = gi->HorizontalResolution;
    uint32_t fh   = gi->VerticalResolution;
    uint32_t fpps = gi->PixelsPerScanLine;
    print("GOP "); printhex(fw); print(" x "); printhex(fh); print(" fb="); printhex(fb); print("\n");

    /* ---- 2. load kernel.bin from the boot volume to phys 0x100000 ---- */
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    BS->HandleProtocol(image, &LIP_GUID, (void **)&li);
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fsp = 0;
    BS->HandleProtocol(li->DeviceHandle, &SFS_GUID, (void **)&fsp);
    EFI_FILE_PROTOCOL *root = 0, *kf = 0;
    fsp->OpenVolume(fsp, &root);
    /* Try the ESP root first (what build.sh stages), then the installer's
     * layout under \EFI\BOLTOS\. A firmware that boots us from a differently
     * organised partition then still finds a kernel instead of dead-ending. */
    static CHAR16 *knames[] = { u"kernel.bin", u"\\EFI\\BOLTOS\\kernel.bin",
                                u"\\EFI\\BOOT\\kernel.bin", u"\\boltos\\kernel.bin" };
    for (unsigned i = 0; i < sizeof(knames) / sizeof(knames[0]); i++) {
        if (root->Open(root, &kf, knames[i], 1, 0) == EFI_SUCCESS) break;
        kf = 0;
    }
    if (!kf) { print("kernel.bin not found on the boot volume\n"); for (;;) {} }

    /* Read the kernel into a scratch buffer. We can't AllocateAddress at 1 MiB
     * (firmware occupies it during boot), so we stage it elsewhere and copy it
     * down to 0x100000 after ExitBootServices, when low RAM is free. */
    uint64_t scratch = alloc_pages(8192);      /* up to 32 MiB kernel */
    if (!scratch) { print("scratch alloc failed\n"); for (;;) {} }
    uint8_t *dst = (uint8_t *)scratch;
    UINTN total = 0;
    for (;;) {
        UINTN chunk = 0x100000;               /* 1 MiB at a time */
        if (kf->Read(kf, &chunk, dst + total) != EFI_SUCCESS) break;
        total += chunk;
        if (chunk == 0) break;
    }
    kf->Close(kf);
    print("kernel loaded: "); printhex(total); print(" bytes\n");

    /* ---- 3. build page tables (8 pages: PML4, 3 PDPTs, 4 PDs) ---- */
    uint64_t tbl = alloc_pages(8);
    if (!tbl) { print("page-table alloc failed\n"); for (;;) {} }
    zero((void *)tbl, 8 * PAGE);
    uint64_t pml4   = tbl;
    uint64_t pdpt_i = tbl + 1 * PAGE;          /* identity   PML4[0]   */
    uint64_t pdpt_d = tbl + 2 * PAGE;          /* direct map PML4[256] */
    uint64_t pdpt_k = tbl + 3 * PAGE;          /* kernel     PML4[511] */
    uint64_t pd0    = tbl + 4 * PAGE;          /* 4 PDs share all PDPTs */

    uint64_t *PML4   = (uint64_t *)pml4;
    uint64_t *PDPT_I = (uint64_t *)pdpt_i;
    uint64_t *PDPT_D = (uint64_t *)pdpt_d;
    uint64_t *PDPT_K = (uint64_t *)pdpt_k;
    PML4[0]   = pdpt_i | 3;
    PML4[256] = pdpt_d | 3;
    PML4[511] = pdpt_k | 3;
    for (int i = 0; i < 4; i++) {
        PDPT_I[i] = (pd0 + (uint64_t)i * PAGE) | 3;
        PDPT_D[i] = (pd0 + (uint64_t)i * PAGE) | 3;
    }
    PDPT_K[510] = pd0 | 3;                      /* 0xFFFFFFFF80000000 -> phys 0 */
    /* fill the 4 PDs: 2048 entries of 2 MiB pages, phys 0..4 GiB */
    uint64_t *PD = (uint64_t *)pd0;
    for (uint64_t i = 0; i < 2048; i++) PD[i] = (i * 0x200000ull) | 0x83;   /* P|RW|PS */

    /* ---- 4. build bootinfo + E820 from the UEFI memory map ---- */
    uint64_t bipg = alloc_pages(4);            /* bootinfo + e820 buffer */
    if (!bipg) { print("bootinfo alloc failed\n"); for (;;) {} }
    zero((void *)bipg, 4 * PAGE);
    struct bootinfo *bi = (struct bootinfo *)bipg;
    struct e820 *e820 = (struct e820 *)(bipg + PAGE);
    bi->fb_addr   = fb;
    bi->fb_width  = fw;
    bi->fb_height = fh;
    bi->fb_pitch  = fpps * 4;                  /* 32 bpp */
    bi->fb_bpp    = 32;
    bi->e820_addr = (uint32_t)(bipg + PAGE);
    bi->boot_flags = bootsel;
    bi->boot_drive = 0xFF;                     /* no BIOS drive number here */

    /* get the memory map (also yields the map key for ExitBootServices) */
    static uint8_t mmbuf[32768];
    UINTN mmsize = sizeof(mmbuf), mapkey = 0, descsz = 0; uint32_t descver = 0;
    if (BS->GetMemoryMap(&mmsize, (EFI_MEMORY_DESCRIPTOR *)mmbuf, &mapkey, &descsz, &descver) != EFI_SUCCESS) {
        print("GetMemoryMap failed\n"); for (;;) {}
    }
    int ne = 0;
    for (UINTN off = 0; off + descsz <= mmsize && ne < 256; off += descsz) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(mmbuf + off);
        uint32_t type = (d->Type == EfiConventionalMemory) ? 1 : 2;   /* usable vs reserved */
        e820[ne].base = d->PhysicalStart;
        e820[ne].len  = d->NumberOfPages * PAGE;
        e820[ne].type = type;
        ne++;
    }
    bi->e820_count = (uint32_t)ne;

    /* ---- 5. exit boot services (refresh the map first; key may change) ---- */
    mmsize = sizeof(mmbuf);
    BS->GetMemoryMap(&mmsize, (EFI_MEMORY_DESCRIPTOR *)mmbuf, &mapkey, &descsz, &descver);
    if (BS->ExitBootServices(image, mapkey) != EFI_SUCCESS) {
        /* the map changed between the two calls: retry once */
        mmsize = sizeof(mmbuf);
        BS->GetMemoryMap(&mmsize, (EFI_MEMORY_DESCRIPTOR *)mmbuf, &mapkey, &descsz, &descver);
        BS->ExitBootServices(image, mapkey);
    }

    /* ---- 6. install our paging, relocate the kernel to 1 MiB, jump ---- *
     * Boot services are gone, so low RAM is now free. Our PML4[0] identity-maps
     * 0..4 GiB, so once CR3 is loaded we can write 0x100000 directly. The copy
     * and jump are done in one asm block (no C calls that might touch freed
     * firmware state). */
    __asm__ volatile(
        "mov %0, %%cr3\n"                 /* activate our page tables          */
        "mov %1, %%rsi\n"                 /* rsi = scratch source              */
        "mov %2, %%rdi\n"                 /* rdi = 0x100000 dest               */
        "mov %3, %%rcx\n"                 /* rcx = byte count                  */
        "rep movsb\n"                     /* copy the kernel down to 1 MiB     */
        "mov %4, %%rdi\n"                 /* arg0 = bootinfo (identity-mapped) */
        "mov %2, %%rax\n"
        "jmp *%%rax\n"
        :: "r"(pml4), "r"(scratch), "r"(KERNEL_PHYS), "r"(total), "r"(bipg)
        : "rax", "rcx", "rsi", "rdi", "memory");

    for (;;) {}
    return EFI_SUCCESS;
}
