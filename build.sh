#!/usr/bin/env bash
# ===========================================================================
#  BoltOS build: stage1 + stage2 (nasm) + kernel (clang/lld) -> raw disk image
# ===========================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

# Detect msys64 root path (works from msys2, Git Bash, and WSL)
M=/c/msys64; [ -d "$M" ] || M=/mnt/c/msys64
[ -d "$M" ] || { echo "ERROR: msys64 not found at /c/msys64 or /mnt/c/msys64"; exit 1; }

export PATH="$M/ucrt64/bin:$M/usr/bin:$PATH"

NASM=$M/usr/bin/nasm.exe
CLANG=$M/ucrt64/bin/clang.exe
LLD=$M/ucrt64/bin/ld.lld.exe
OBJCOPY=$M/ucrt64/bin/objcopy.exe

CFLAGS=(--target=x86_64-elf -ffreestanding -fno-pic -fno-pie
        -mcmodel=kernel
        -mno-red-zone -msse -msse2 -mno-mmx -mno-80387
        -fstack-protector-strong -mstack-protector-guard=global
        -Wall -Wextra -O2 -std=c11 -Iinclude -c)

# Locate a host Python (mkboothdr.py + mkhpfs.py). $BOLTOS_PYTHON wins,
# otherwise every interpreter on PATH and in the usual Windows install roots is
# tried, under both msys (/c) and WSL (/mnt/c) mount styles. Hard-coding one
# machine's Python311 path made a fresh checkout fail at the very last step.
#
# A machine can easily have several Pythons (msys2 ships one of its own), and
# only some will have pycdlib, which mkhpfs.py needs for the ISO. So candidates
# that can import it are preferred; the first that merely runs is the fallback,
# and the ISO step then reports what to install.
py_candidates() {
    local c
    # NB: not the `py` launcher -- it re-dispatches on the script's shebang, so
    # `py -c "import pycdlib"` can succeed while `py mkhpfs.py` picks a
    # different interpreter that lacks it.
    for c in python3 python; do command -v "$c" 2>/dev/null; done
    ls -1 /c/Python3*/python.exe /mnt/c/Python3*/python.exe           /c/Users/*/AppData/Local/Programs/Python/Python3*/python.exe           /mnt/c/Users/*/AppData/Local/Programs/Python/Python3*/python.exe 2>/dev/null
}
PYTHON="${BOLTOS_PYTHON-}"
PYTHON_FALLBACK=""
if [ -z "$PYTHON" ]; then
    while IFS= read -r c; do
        [ -x "$c" ] || continue
        [ -n "$PYTHON_FALLBACK" ] || PYTHON_FALLBACK="$c"
        if "$c" -c "import pycdlib" >/dev/null 2>&1; then PYTHON="$c"; break; fi
    done <<< "$(py_candidates)"
    [ -n "$PYTHON" ] || PYTHON="$PYTHON_FALLBACK"
fi
[ -n "$PYTHON" ] || { echo "ERROR: no host Python found (set BOLTOS_PYTHON)"; exit 1; }

mkdir -p build iso

echo "[1/6] boot chain (stage1 + stage2 + BOOTX64.EFI)"
# The whole boot chain is built BEFORE the kernel now, and embedded into the
# kernel image as blobs. That is what lets `install` write a bootable BoltOS
# onto another disk from inside BoltOS -- the running system carries its own
# bootloader. It is only possible because stage2 no longer needs the kernel's
# size baked in: it reads the boot header at LBA 33 (include/boot.h) instead.
"$NASM" -f bin boot/stage1.asm -o build/stage1.bin
"$NASM" -f bin boot/stage2.asm -o build/stage2.bin
S2SIZE=$(stat -c %s build/stage2.bin)
if [ "$S2SIZE" -gt $((32 * 512)) ]; then
    echo "ERROR: stage2 is $S2SIZE bytes, exceeds 16384 (32 sectors)"; exit 1
fi

# A genuine UEFI application (PE32+) that loads the kernel on CSM-less firmware.
# MS ABI (which UEFI uses); no red zone; SSE off so it runs before the kernel
# turns the FPU on. Linked by lld-link as an EFI Application.
UEFI_CFLAGS=(--target=x86_64-unknown-windows -ffreestanding -fno-stack-protector
             -fshort-wchar -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-80387
             -Wall -O2 -Iinclude -c)
"$CLANG" "${UEFI_CFLAGS[@]}" boot/uefi_boot.c -o build/uefi_boot.o
"$M/ucrt64/bin/lld-link.exe" -subsystem:efi_application -entry:efi_main     -out:build/BOOTX64.EFI build/uefi_boot.o

# Blobs the installer writes verbatim onto a target disk / ESP.
( cd build && "$OBJCOPY" -I binary -O elf64-x86-64 stage1.bin    stage1_blob.o )
( cd build && "$OBJCOPY" -I binary -O elf64-x86-64 stage2.bin    stage2_blob.o )
( cd build && "$OBJCOPY" -I binary -O elf64-x86-64 BOOTX64.EFI   bootx64_blob.o )

echo "[2/6] kernel asm (boot + isr + syscall + AP trampoline)"
"$NASM" -f elf64 kernel/boot.asm    -o build/kboot.o
"$NASM" -f elf64 kernel/isr.asm     -o build/isr.o
"$NASM" -f elf64 kernel/syscall.asm -o build/sc_entry.o
# SMP application-processor trampoline: flat 16/32/64-bit blob copied to 0x8000
"$NASM" -f bin kernel/ap_boot.asm -o build/ap_boot.bin
( cd build && "$OBJCOPY" -I binary -O elf64-x86-64 ap_boot.bin ap_boot_blob.o )

echo "[2b/6] user program (user/hello.c) -> static ELF64 -> embed blob"
UCFLAGS=(--target=x86_64-elf -ffreestanding -fno-stack-protector -fno-pic -fno-pie
         -mno-red-zone -msse -msse2 -mno-mmx -mno-80387
         -Wall -Wextra -O2 -std=c11 -Iuser -Iinclude -c
         # Frame pointers are kept in ring 3 on purpose: a core dump's
         # backtrace is walked from RBP, and a program you cannot get a
         # backtrace out of is one you cannot debug.
         -fno-omit-frame-pointer)
"$NASM" -f elf64 user/crt0.asm -o build/u_crt0.o
"$CLANG" "${UCFLAGS[@]}" user/ulibc.c      -o build/u_ulibc.o
"$CLANG" "${UCFLAGS[@]}" user/stdlib_ext.c -o build/u_stdlib.o
"$CLANG" "${UCFLAGS[@]}" user/libm.c       -o build/u_libm.o
"$CLANG" "${UCFLAGS[@]}" user/hello.c      -o build/u_hello.o
"$CLANG" "${UCFLAGS[@]}" libc/string.c     -o build/u_string.o
"$LLD" -m elf_x86_64 -T user/user.ld -no-pie -o build/hello.elf \
       build/u_crt0.o build/u_hello.o build/u_ulibc.o build/u_stdlib.o \
       build/u_libm.o build/u_string.o
# embed the raw ELF as an object exposing _binary_hello_elf_start/_end
( cd build && "$OBJCOPY" -I binary -O elf64-x86-64 hello.elf hello_blob.o )

echo "[2b2/6] RING-3 engines: JS interpreter + web browser (DOM+layout+JS) -> ELF64"
# The browser, DOM/layout engine and JS interpreter, compiled into userland ELFs
# and run in RING 3. kernel/{js,dom,layout,font8x8}.c are the SAME sources the
# kernel uses, but here built with the user flags (-Iuser first, so kheap.h is
# the malloc shim) and linked against ulibc -- no part runs in ring 0. They reach
# the framebuffer / keyboard / network only through syscalls.
"$CLANG" "${UCFLAGS[@]}" libc/string_ext.c -o build/u_string_ext.o
"$CLANG" "${UCFLAGS[@]}" kernel/js.c        -o build/u_js.o
"$CLANG" "${UCFLAGS[@]}" kernel/dom.c       -o build/u_dom.o
"$CLANG" "${UCFLAGS[@]}" kernel/layout.c    -o build/u_layout.o
"$CLANG" "${UCFLAGS[@]}" kernel/font8x8.c   -o build/u_font8x8.o
"$CLANG" "${UCFLAGS[@]}" user/crashme.c      -o build/u_crashme.o
"$CLANG" "${UCFLAGS[@]}" user/netget.c       -o build/u_netget.o
"$CLANG" "${UCFLAGS[@]}" user/js_main.c      -o build/u_js_main.o
"$CLANG" "${UCFLAGS[@]}" user/browser_main.c -o build/u_browser_main.o
UCORE=(build/u_ulibc.o build/u_stdlib.o build/u_libm.o build/u_string.o build/u_string_ext.o)
# /bin/js -- the JavaScript interpreter as a ring-3 program
"$LLD" -m elf_x86_64 -T user/user.ld -no-pie -o build/js.elf \
       build/u_crt0.o build/u_js_main.o build/u_js.o "${UCORE[@]}"
( cd build && "$OBJCOPY" -I binary -O elf64-x86-64 js.elf js_blob.o )
# /bin/browser -- the web browser (DOM + layout + JS) as a ring-3 program
"$LLD" -m elf_x86_64 -T user/user.ld -no-pie -o build/browser.elf \
       build/u_crt0.o build/u_browser_main.o build/u_js.o build/u_dom.o \
       build/u_layout.o build/u_font8x8.o "${UCORE[@]}"
( cd build && "$OBJCOPY" -I binary -O elf64-x86-64 browser.elf browser_blob.o )
# /bin/crashme -- faults on purpose, so per-process fault isolation can be shown
# and regression tested (`crash ring3`). Tiny; links only the user core.
"$LLD" -m elf_x86_64 -T user/user.ld -no-pie -o build/crashme.elf \
       build/u_crt0.o build/u_crashme.o "${UCORE[@]}"
( cd build && "$OBJCOPY" -I binary -O elf64-x86-64 crashme.elf crashme_blob.o )
# /bin/netget -- fetches a URL over a ring-3 socket, so the socket syscalls are
# exercised by a real user program rather than only from the kernel.
"$LLD" -m elf_x86_64 -T user/user.ld -no-pie -o build/netget.elf build/u_crt0.o build/u_netget.o "${UCORE[@]}"
( cd build && "$OBJCOPY" -I binary -O elf64-x86-64 netget.elf netget_blob.o )

echo "[2c/6] windows program (user/winhello.c) -> real PE32+ EXE -> embed blob"
# A genuine Windows x86-64 console .exe: freestanding, imports kernel32, linked
# by the mingw toolchain. BoltOS's PE loader binds the imports to its shim.
# NB: built with SSE/MMX/x87 disabled to match the kernel (which runs with SSE
# off), so the PE contains only general-purpose-register code the loader can run.
"$CLANG" --target=x86_64-w64-mingw32 -nostdlib -ffreestanding -fno-stack-protector \
         -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mno-red-zone \
         -O2 -Wl,-e,entry -Wl,--subsystem,console \
         user/winhello.c -o build/winhello.exe -lkernel32
( cd build && "$OBJCOPY" -I binary -O elf64-x86-64 winhello.exe winhello_blob.o )

echo "[2d/6] BoltRT stub: BoltCC + BoltPython baked into a PE32+ EXE -> embed blob"
# The runtime stub that `compile` patches with user source to make a real .exe.
# It links the actual BoltCC / BoltPython front-ends into a freestanding Windows
# console PE (imports kernel32, runs on Win11 AND under BoltOS's PE loader).
# -mstack-probe-size is set huge so clang never emits __chkstk for the compilers'
# large stack frames (no libgcc under -nostdlib).
"$CLANG" --target=x86_64-w64-mingw32 -nostdlib -ffreestanding -fno-stack-protector \
         -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mno-red-zone \
         -mstack-probe-size=0x7fffffff \
         -O2 -std=c11 -DBOLTCC_NO_DEBUGGER -Iinclude -Wl,-e,entry -Wl,--subsystem,console -Wl,--dynamicbase \
         user/boltrt.c kernel/boltcc.c kernel/ccnative.c kernel/boltpy.c libc/string.c \
         -o build/boltrt.exe -lkernel32
( cd build && "$OBJCOPY" -I binary -O elf64-x86-64 boltrt.exe boltrt_blob.o )

echo "[3/6] kernel C sources"
SRCS=(
    kernel/cmd_run.c kernel/manpages.c kernel/shline.c kernel/shscript.c
    kernel/notify.c kernel/cmd_notify.c kernel/png_enc.c kernel/screenshot.c kernel/cmd_shot.c
    kernel/cmd_desk.c kernel/keymap.c kernel/cmd_keys.c kernel/app_software.c kernel/cmd_logs.c kernel/app_logs.c
    kernel/main.c kernel/serial.c kernel/console.c kernel/shell.c kernel/kprintf.c kernel/font8x8.c
    kernel/gdt.c kernel/idt.c kernel/interrupts.c kernel/pic.c kernel/pit.c
    kernel/acpi.c kernel/apic.c kernel/hpet.c kernel/smp.c
    kernel/hw.c kernel/pci.c kernel/sysreg.c kernel/sched.c kernel/syscall.c
    kernel/vfs.c kernel/proc.c kernel/elf.c kernel/pe.c kernel/blk.c
    net/netif.c net/driver.c net/eth.c net/arp.c net/ip.c net/icmp.c net/udp.c net/netaddr.c
    net/tcp.c net/dns.c net/crypto.c net/tls.c net/p256.c net/p384.c net/rsa.c net/x509.c net/inflate.c net/http.c
    net/ip6.c net/icmp6.c net/nd6.c
    net/wifi.c net/wpa.c net/wifi_sta.c net/firmware.c net/firewall.c net/dhcp.c net/netcfg.c net/ntp.c
    drivers/e1000.c drivers/virtio_net.c drivers/wifi_sim.c
    kernel/socket.c net/ssh.c kernel/cmd_ssh.c kernel/cmd_edit.c kernel/cmd_netcfg.c kernel/cmd_serve.c kernel/cmd_fs.c kernel/cmd_sys.c kernel/cmd_proc.c kernel/cmd_net.c kernel/cmd_extra.c kernel/cmd_hw.c kernel/cmd_user.c
    kernel/html.c kernel/dom.c kernel/layout.c kernel/image.c kernel/ttf.c
    kernel/gui.c kernel/app_terminal.c kernel/app_taskmgr.c kernel/app_settings.c kernel/app_browser.c kernel/app_files.c
    kernel/app_oldbrowser.c oldbrowser/ob_nsurl.c oldbrowser/ob_llcache.c oldbrowser/ob_content.c oldbrowser/ob_window.c oldbrowser/ob_hotlist.c oldbrowser/ob_fbtk.c oldbrowser/ob_gui.c
    kernel/app_ide.c kernel/app_calc.c kernel/app_clock.c kernel/app_notes.c kernel/app_tasks.c kernel/app_contacts.c kernel/app_sheets.c kernel/app_imgview.c kernel/app_music.c kernel/app_media.c kernel/cmd_media.c kernel/app_calendar.c kernel/app_piano.c kernel/app_paint.c kernel/app_mines.c kernel/app_snake.c kernel/app_2048.c kernel/app_stopwatch.c kernel/app_sysinfo.c kernel/app_life.c kernel/app_ttt.c kernel/app_colorpick.c kernel/app_memory.c kernel/app_matrix.c kernel/app_doom.c kernel/app_installer.c kernel/app_security.c
    kernel/core.c kernel/cmd_core.c kernel/pkgcat.c kernel/settings.c kernel/clipboard.c kernel/stackguard.c kernel/users.c kernel/vault.c kernel/cmd_vault.c kernel/pkg.c kernel/install.c kernel/klog.c
    kernel/asm.c kernel/cmd_asm.c kernel/ccnative.c
    kernel/cpp.c kernel/ccbuild.c kernel/cmd_cc.c kernel/cmd_build.c kernel/cmd_vcs.c kernel/boltpy.c kernel/cmd_python.c kernel/boltcc.c kernel/js.c kernel/cmd_js.c
    fs/ramfs.c fs/fat32.c fs/ext2.c
    drivers/keyboard.c drivers/framebuffer.c drivers/gpu.c drivers/mouse.c drivers/ata.c drivers/ahci.c drivers/nvme.c drivers/xhci.c drivers/pcspk.c drivers/audio.c drivers/ac97.c drivers/hda.c mm/pmm.c mm/vmm.c mm/kheap.c mm/dma.c
    kernel/wav.c kernel/cmd_libctest.c
)
# The freestanding C library. Built with -fno-math-errno/-fno-trapping-math so the
# compiler lowers __builtin_sqrt &c. to bare SSE instructions instead of an
# errno-aware library call -- otherwise `double sqrt(x){return __builtin_sqrt(x);}`
# tail-calls itself into an infinite loop.
LIBC_SRCS=(
    libc/string.c libc/string_ext.c libc/malloc.c libc/printf.c libc/scanf.c
    libc/stdlib.c libc/support.c libc/math.c libc/stdio_file.c libc/time.c
)
KOBJS=(build/kboot.o build/isr.o build/sc_entry.o build/ap_boot_blob.o build/hello_blob.o build/js_blob.o build/browser_blob.o build/winhello_blob.o build/boltrt_blob.o build/crashme_blob.o build/netget_blob.o
       build/stage1_blob.o build/stage2_blob.o build/bootx64_blob.o)
# libc non-local jump (asm); clang's integrated assembler handles the .S
"$CLANG" --target=x86_64-elf -mcmodel=kernel -c libc/setjmp.S -o build/setjmp.o
KOBJS+=(build/setjmp.o)
for c in "${LIBC_SRCS[@]}"; do
    o="build/$(basename "${c%.c}").o"
    "$CLANG" "${CFLAGS[@]}" -fno-math-errno -fno-trapping-math "$c" -o "$o"
    KOBJS+=("$o")
done
for c in "${SRCS[@]}"; do
    o="build/$(basename "${c%.c}").o"
    "$CLANG" "${CFLAGS[@]}" "$c" -o "$o"
    KOBJS+=("$o")
done

echo "[3b/6] DOOM engine (from-scratch doomgeneric port -> kernel objects)"
# The id Tech 1 engine, built against the in-house libc shim (doom/dg_libc.c) and
# BoltOS platform layer (doom/doomgeneric_boltos.c). -nostdlibinc keeps clang's
# freestanding headers but blocks the host's libc headers, so <stdio.h> etc.
# resolve to doom/libc/*. SSE2 stays on (the engine uses doubles at table build
# time); -w because vintage code is warning-heavy.
DOOMCFLAGS=(--target=x86_64-elf -ffreestanding -fno-stack-protector -fno-pic -fno-pie
            -mcmodel=kernel -mno-red-zone -msse -msse2 -mno-mmx -mno-80387
            -O2 -w -std=gnu11 -nostdlibinc -Idoom/libc -Idoom -Iinclude
            -DNORMALUNIX -DLINUX -c)
# DOOM ships its own private libc shim (doom/dg_libc.c). Now that the kernel has
# a real freestanding libc (libc/*.c) exporting the same standard names, those
# would collide at the final link. Namespace every symbol dg_libc.c defines to a
# dg_ prefix (token-level, so DOOM stays self-consistent and keeps its exact
# arena/WAD-IO behaviour) and leave the global libc names to the real libc.
# The exact set (computed by intersecting nm of dg_libc.o with the real libc).
for s in abort abs atan atan2 atof atol calloc ceil cos errno exit exp fabs \
         fabsf fclose feof ferror fflush fgetc fgets fileno floor fmod fopen \
         fprintf fputc fputs fread free fseek ftell fwrite getenv labs log \
         malloc memchr pow printf putchar puts qsort rand realloc remove rename \
         rewind sin snprintf sprintf sqrt sqrtf srand sscanf stderr stdin stdout \
         strcasecmp strdup strerror strncasecmp strncat strstr strtod strtol \
         strtoul system tan vfprintf vprintf vsnprintf vsprintf; do
    DOOMCFLAGS+=("-D${s}=dg_${s}")
done
DOOM_SRCS=(
    dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net
    f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale i_sound
    i_system i_timer memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc
    m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats
    p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw
    r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables
    v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc i_input i_video
    doomgeneric doomgeneric_boltos dg_libc
)
for d in "${DOOM_SRCS[@]}"; do
    "$CLANG" "${DOOMCFLAGS[@]}" "doom/$d.c" -o "build/doom_$d.o"
    KOBJS+=("build/doom_$d.o")
done

echo "[3c/6] embed shareware DOOM IWAD (doom1.wad -> blob)"
[ -f doom/doom1.wad ] || { echo "ERROR: doom/doom1.wad missing (shareware IWAD)"; exit 1; }
cp doom/doom1.wad build/doom1.wad
( cd build && "$OBJCOPY" -I binary -O elf64-x86-64 doom1.wad doom1_wad.o )
KOBJS+=(build/doom1_wad.o)

echo "[3d/6] embed a TrueType font (font.ttf -> blob)"
# A monospace TrueType face for the scalable/anti-aliased text renderer. Copied
# from the host's Lucida Console if present; the kernel falls back to its 8x16
# bitmap face when no glyf-based TTF is embedded.
if [ ! -f assets/fonts/font.ttf ]; then
    mkdir -p assets/fonts
    for c in /c/Windows/Fonts/lucon.ttf /c/Windows/Fonts/cour.ttf /mnt/c/Windows/Fonts/lucon.ttf; do
        [ -f "$c" ] && { cp "$c" assets/fonts/font.ttf; break; }
    done
fi
[ -f assets/fonts/font.ttf ] || { echo "WARN: no font.ttf; embedding an empty blob"; : > assets/fonts/font.ttf; }
cp assets/fonts/font.ttf build/font.ttf
( cd build && "$OBJCOPY" -I binary -O elf64-x86-64 font.ttf font_ttf.o )
KOBJS+=(build/font_ttf.o)

echo "[3d2/6] generate the system sounds (chime.wav, blip.wav -> blobs)"
# Two shapes on purpose: 22 kHz mono 8-bit and 44.1 kHz stereo 16-bit. Between
# them they make the decoder resample, expand mono to stereo and sign-convert,
# which is what any WAV off a real disk asks of it.
"$PYTHON" mkwav.py assets/audio
cp assets/audio/chime.wav assets/audio/blip.wav build/
( cd build && "$OBJCOPY" -I binary -O elf64-x86-64 chime.wav chime_wav.o )
( cd build && "$OBJCOPY" -I binary -O elf64-x86-64 blip.wav  blip_wav.o  )
KOBJS+=(build/chime_wav.o build/blip_wav.o)

echo "[3d3/6] embed the desktop wallpapers (one render per resolution -> blobs)"
# The compositor stretches the wallpaper with a nearest-neighbour sampler, so a
# single file would be stair-stepped at four of the five modes. mkwall.py cuts
# the 4K master down to one JPEG per entry in settings.c's res_tab[]; the whole
# set is ~1.5 MB, where the same renders as PNG would be 13.5 MB and the 4K one
# alone would want ~99 MiB of kheap to decode.
WALLPAPERS=(assets/wallpapers/mountain-*.jpg)
[ -e "${WALLPAPERS[0]}" ] || {
    echo "ERROR: no assets/wallpapers/mountain-*.jpg -- run: python mkwall.py assets/wallpapers/mountain.png assets/wallpapers"
    exit 1
}
for w in "${WALLPAPERS[@]}"; do
    b=$(basename "$w")
    cp "$w" "build/$b"
    o="${b//[-.]/_}.o"
    ( cd build && "$OBJCOPY" -I binary -O elf64-x86-64 "$b" "$o" )
    KOBJS+=("build/$o")
done

echo "[3e/6] real NetSurf libraries (upstream source, compiled against the libc)"
# The genuine NetSurf support stack, vendored under netsurf/ and built against
# BoltOS's freestanding libc. Each library keeps its real upstream source; only
# the platform glue (allocators, logging) is supplied by the libc + shims. The
# include search path and source list grow as libraries are brought up in
# dependency order: libwapcaplet -> libparserutils -> libhubbub -> libcss -> libdom.
LIBCSS=vendor/libcss-0.9.2
LIBDOM=vendor/libdom-0.4.1
# Public (namespaced) include dirs are safe to share: every header lives under a
# library prefix (parserutils/..., libcss/..., dom/...) so they never collide.
# The PRIVATE src/ trees are NOT safe to share -- libparserutils, libhubbub and
# libcss each ship a "utils/utils.h", so a quoted #include "utils/utils.h"
# resolves to whichever src/ comes first on -I. The fix: every library is
# compiled with its OWN src/ prepended, ahead of the shared public set, so each
# picks up its own internal headers. (This mirrors upstream's per-library build.)
# NB: libdom's bindings/ dir is deliberately NOT on the include path -- it ships
# its own <hubbub/errors.h>/<hubbub/parser.h> that would shadow libhubbub's; the
# binding TU instead reaches them as quoted includes relative to its own dir.
NS_PUB=(-Iinclude -Inetsurf
        -Inetsurf/libwapcaplet/include
        -Inetsurf/libparserutils/include
        -Inetsurf/libhubbub/include
        -I$LIBCSS/include
        -I$LIBDOM/include)
NSCFLAGS_BASE=(--target=x86_64-elf -ffreestanding -fno-stack-protector -fno-pic -fno-pie
          -mcmodel=kernel -mno-red-zone -msse -msse2 -mno-mmx -mno-80387
          -fno-math-errno -fno-trapping-math -O2 -w -std=gnu11
          -DWITHOUT_ICONV_FILTER
          # Release semantics for the vendored libs: disable asserts (no
          # __assert_fail dependency) and silence libhubbub's treebuilder debug
          # hook (treebuilder.c prints each insertion-mode name unless NDEBUG).
          -DNDEBUG
          # libcss's stylesheet.h tags struct css_rule with a trailing `_ALIGNED`
          # macro that 0.9.2 ships undefined -- under old GCC's -fcommon it merged
          # as a tentative definition, but modern clang (-fno-common) emits a
          # strong global per TU => "duplicate symbol _ALIGNED" at link. Defining
          # it empty turns `} _ALIGNED;` back into a pure struct type definition.
          -D_ALIGNED=)
# ns_cc <private-inc-dir-or-empty> <src...> : compile each src with the given
# library-private src/ dir first, then the shared public includes.
ns_cc() {
    local priv="$1"; shift
    local privflag=(); local p
    for p in $priv; do privflag+=(-I"$p"); done   # $priv may list several dirs
    for c in "$@"; do
        local o="build/ns_$(echo "$c" | sed 's#[/.]#_#g').o"
        "$CLANG" "${NSCFLAGS_BASE[@]}" "${privflag[@]}" "${NS_PUB[@]}" -c "$c" -o "$o"
        KOBJS+=("$o")
    done
}

# libwapcaplet (string interning) -- single TU, no private headers needed.
ns_cc "" netsurf/libwapcaplet/src/libwapcaplet.c

# libparserutils (buffers, charset codecs, input streams).
ns_cc netsurf/libparserutils/src \
    netsurf/libparserutils/src/utils/buffer.c \
    netsurf/libparserutils/src/utils/errors.c \
    netsurf/libparserutils/src/utils/stack.c \
    netsurf/libparserutils/src/utils/vector.c \
    netsurf/libparserutils/src/charset/aliases.c \
    netsurf/libparserutils/src/charset/codec.c \
    netsurf/libparserutils/src/charset/codecs/codec_8859.c \
    netsurf/libparserutils/src/charset/codecs/codec_ascii.c \
    netsurf/libparserutils/src/charset/codecs/codec_ext8.c \
    netsurf/libparserutils/src/charset/codecs/codec_utf16.c \
    netsurf/libparserutils/src/charset/codecs/codec_utf8.c \
    netsurf/libparserutils/src/charset/encodings/utf16.c \
    netsurf/libparserutils/src/charset/encodings/utf8.c \
    netsurf/libparserutils/src/input/filter.c \
    netsurf/libparserutils/src/input/inputstream.c

# libhubbub (HTML5 tokeniser + tree builder). Every source except the textually
# -#included gperf replacement (autogenerated-element-type.c is pulled in by
# element-type.c, not compiled standalone).
# NB: source lists are gathered with a single `find` in a command substitution
# (no pipe into `while read` via process substitution). msys2's fork()/pipe
# emulation can intermittently truncate such a pipeline, silently dropping source
# files -> undefined symbols at link. A lone find + word-split (these vendor paths
# contain no spaces) is deterministic; a count guard catches any truncation.
HB_SRCS=( $(find netsurf/libhubbub/src -name '*.c' ! -name 'autogenerated-element-type.c') )
[ "${#HB_SRCS[@]}" -ge 20 ] || { echo "ERROR: libhubbub scan truncated (${#HB_SRCS[@]})"; exit 1; }
ns_cc netsurf/libhubbub/src "${HB_SRCS[@]}"

# libcss codegen: ~120 "simple" property parsers (css__parse_<prop>) are emitted
# at build time. A host tool (css_property_parser_gen.c) reads one descriptor
# line per property from properties.gen and writes autogenerated_<prop>.c into
# the properties dir. Upstream drives this from its Makefile; we replicate it so
# the css__parse_* symbols the parser dispatch table references actually exist.
# (css_property_parser_gen.c is a HOST program -- stdio FILE* -- so it is built
# with the native compiler and never linked into the kernel.)
CSSPROPDIR=$LIBCSS/src/parse/properties
echo "      generating libcss property parsers (autogenerated_*.c)"
"$CLANG" -O2 -w -o build/gen_parser.exe "$CSSPROPDIR/css_property_parser_gen.c"
# Read properties.gen DIRECTLY (a plain file redirect -- no pipeline, no process
# substitution) and extract the property name with pure-shell expansion. Earlier
# revisions piped grep|sed|sort into `while read`, but msys2's fork()/pipe
# emulation intermittently truncated that pipeline to a single line, leaving most
# autogenerated_*.c empty -> a flood of undefined css__parse_* at link. Spawning
# only gen_parser per line (with stdin closed so it can't drain the loop) makes
# this deterministic. Each descriptor is one line: "<name>:<descriptor...>".
gen_n=0
while IFS= read -r gline || [ -n "$gline" ]; do
    case "$gline" in \#*|"") continue;; esac      # skip comments / blank lines
    case "$gline" in *:*) ;; *) continue;; esac   # must be a "name:..." line
    name="${gline%%:*}"                            # text before the first ':'
    [ -z "$name" ] && continue
    build/gen_parser.exe -o "$CSSPROPDIR/autogenerated_${name}.c" "$gline" </dev/null
    gen_n=$((gen_n + 1))
done < "$CSSPROPDIR/properties.gen"
echo "      generated $gen_n property parsers"
if [ "$gen_n" -lt 100 ]; then
    echo "ERROR: only $gen_n libcss property parsers generated (expected ~119)"; exit 1
fi

# libcss (CSS lexer + parser + computed-style selection). The whole src/ tree
# except css_property_parser_gen.c, which is the HOST generator above -- never
# compiled into the kernel. The autogenerated_*.c just produced ARE picked up.
CSS_SRCS=( $(find $LIBCSS/src -name '*.c' ! -name 'css_property_parser_gen.c') )
[ "${#CSS_SRCS[@]}" -ge 180 ] || { echo "ERROR: libcss scan truncated (${#CSS_SRCS[@]})"; exit 1; }
ns_cc $LIBCSS/src "${CSS_SRCS[@]}"

# libdom (W3C DOM core/events/html + the hubbub binding that turns a libhubbub
# parse into a real DOM tree -- this IS NetSurf's genuine HTML->DOM path). The
# whole src/ tree compiles cleanly; the binding TU is built separately because it
# lives outside src/ and pulls its private headers ("parser.h"/"utils.h") as
# quoted includes relative to bindings/hubbub/.
DOM_SRCS=( $(find $LIBDOM/src -name '*.c') )
[ "${#DOM_SRCS[@]}" -ge 80 ] || { echo "ERROR: libdom scan truncated (${#DOM_SRCS[@]})"; exit 1; }
ns_cc $LIBDOM/src "${DOM_SRCS[@]}"
ns_cc $LIBDOM/src $LIBDOM/bindings/hubbub/parser.c

# BoltOS-side selection glue (real libcss<->libdom handler) + test/driver. Both
# need private dirs: libhubbub's src/ (the low-level tokeniser test), and the
# libdom hubbub binding dir (for "parser.h"). The binding dir exposes parser.h
# directly, NOT a hubbub/ subdir, so it does not shadow libhubbub's headers.
ns_cc "" oldbrowser/ns_select.c
ns_cc "$LIBDOM/bindings/hubbub" oldbrowser/ns_html.c
ns_cc "netsurf/libhubbub/src $LIBDOM/bindings/hubbub" kernel/cmd_nstest.c

echo "[4/6] link kernel -> flat binary"
# The object set (kernel + DOOM + the full NetSurf stack: libwapcaplet,
# libparserutils, libhubbub, libcss, libdom = ~600 .o) overflows the Windows
# command-line length limit, so feed the list to lld via a response file (@file).
printf '%s\n' "${KOBJS[@]}" > build/kobjs.rsp
"$LLD" -m elf_x86_64 -T linker.ld --oformat binary -o build/kernel.bin @build/kobjs.rsp
# Also emit an ELF with symbols (same objects/order) for debugging/addr2line.
"$LLD" -m elf_x86_64 -T linker.ld -o build/kernel.elf @build/kobjs.rsp 2>/dev/null || true

KSIZE=$(stat -c %s build/kernel.bin)
KSECT=$(( (KSIZE + 511) / 512 ))
echo "      kernel.bin = $KSIZE bytes -> $KSECT sectors"

echo "[5/6] assemble disk image"
# Disk layout (mirrored exactly by the `install` command):
#   LBA 0      stage1 (MBR)
#   LBA 1..32  stage2
#   LBA 33     BoltOS boot header -- tells stage2 where the kernel is and how
#              many sectors to load, so stage2 itself is kernel-independent
#   LBA 34..   kernel.bin
IMG=iso/os.img
TOTAL=$(( 34 + KSECT ))
dd if=/dev/zero          of="$IMG" bs=512 count="$TOTAL"      status=none
dd if=build/stage1.bin   of="$IMG" bs=512 seek=0  conv=notrunc status=none
dd if=build/stage2.bin   of="$IMG" bs=512 seek=1  conv=notrunc status=none
dd if=build/kernel.bin   of="$IMG" bs=512 seek=34 conv=notrunc status=none
"$PYTHON" mkboothdr.py "$IMG" 34 "$KSIZE"

echo "OK -> $IMG ($(stat -c %s "$IMG") bytes)"

# Persistent data disks for the filesystem. Created once and left alone on
# rebuilds so files survive across runs. One is presented to QEMU as a spinning
# HDD, the other as an SSD (rotation_rate flag in run.sh) - the ATA driver tells
# them apart from the IDENTIFY page. 64 MiB each.
DATA_MB=64
for d in iso/disk-hdd.img iso/disk-ssd.img iso/disk-nvme.img; do
    if [ ! -f "$d" ]; then
        dd if=/dev/zero of="$d" bs=1M count="$DATA_MB" status=none
        echo "created $d (${DATA_MB} MiB)"
    fi
done
# Blank SATA disk that BoltOS formats as FAT32 on first boot (mounted via AHCI).
if [ ! -f iso/disk-fat.img ]; then
    dd if=/dev/zero of=iso/disk-fat.img bs=1M count="$DATA_MB" status=none
    echo "created iso/disk-fat.img (${DATA_MB} MiB, FAT32 at first boot)"
fi
# A real ext2 volume (Linux-formatted) so BoltOS's ext2 driver has something to
# read. Built with mke2fs -d (populate without mounting) via WSL when present;
# otherwise a blank image is left and the driver simply finds no ext2 here.
if [ ! -f iso/disk-ext2.img ]; then
    # translate the repo's /c/... (msys) path to WSL's /mnt/c/... for mke2fs
    WSL_IMG="$(echo "$ROOT/iso/disk-ext2.img" | sed -E 's#^/([a-zA-Z])/#/mnt/\1/#')"
    if command -v wsl.exe >/dev/null 2>&1 && \
       wsl.exe -e bash -c "command -v mke2fs >/dev/null && S=/tmp/boltext2 && rm -rf \$S && mkdir -p \$S/docs && \
         printf 'Hello from a real ext2 filesystem!\nBoltOS read this with its own ext2 driver.\n' > \$S/README.txt && \
         printf 'line one\nline two\nline three\n' > \$S/docs/notes.txt && \
         printf 'BoltOS ext2 test\n' > \$S/HELLO && \
         mke2fs -q -t ext2 -b 1024 -d \$S -F '$WSL_IMG' 4096" >/dev/null 2>&1; then
        echo "created iso/disk-ext2.img (real ext2 via mke2fs)"
    else
        dd if=/dev/zero of=iso/disk-ext2.img bs=1M count=4 status=none
        echo "created iso/disk-ext2.img (blank; install WSL+e2fsprogs for a real ext2)"
    fi
fi

echo "[7/7] build bootable ISO"
ISOPATH=iso/boltos.iso
if ! "$PYTHON" -c "import pycdlib" >/dev/null 2>&1; then
    echo "ERROR: $PYTHON has no pycdlib (needed for the ISO)."
    echo "       install it with:  $PYTHON -m pip install pycdlib"
    echo "       iso/os.img is already built and bootable without it."
    exit 1
fi
"$PYTHON" mkhpfs.py "$IMG" "$ISOPATH" "BoltOS"
echo "OK -> $ISOPATH ($(stat -c %s "$ISOPATH") bytes)"

echo "[8/8] ESP staging (BOOTX64.EFI was built in step 1)"
# Stage an EFI System Partition tree: firmware auto-runs EFI/BOOT/BOOTX64.EFI,
# which reads kernel.bin from the same volume. QEMU can serve esp/ as a FAT
# volume directly (-drive ...,file=fat:rw:esp), so no disk image needed.
mkdir -p esp/EFI/BOOT
cp build/BOOTX64.EFI esp/EFI/BOOT/BOOTX64.EFI
cp build/kernel.bin  esp/kernel.bin
echo "OK -> esp/ (BOOTX64.EFI $(stat -c %s build/BOOTX64.EFI) bytes + kernel.bin)"
