#!/usr/bin/env bash
# ===========================================================================
#  BoltOS Mini  -  mini/firefox/build.sh
#
#  Builds what BoltOS Mini runs as its Firefox machine (mini/vm.c): a Linux
#  kernel with everything built in that a virtio-mmio guest on BoltOS needs,
#  and a read-only root filesystem -- Debian, Firefox from Mozilla, cage (a
#  Wayland compositor that shows one window, full screen) and Windows'
#  fonts' open stand-ins -- as squashfs.
#
#  Runs as root on an arm64 Debian (the engine VM: make-image.sh starts it,
#  copies this folder in and runs this). Writes to /opt/boltfx/out:
#      Image          the kernel, uncompressed
#      root.sqfs      the root filesystem
#
#      sudo bash build.sh kernel | root | all
# ===========================================================================
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK=/opt/boltfx
OUT=$WORK/out
LINUX=6.18.59
mkdir -p "$WORK" "$OUT"
export DEBIAN_FRONTEND=noninteractive

kernel() {
    apt-get install -y -q --no-install-recommends build-essential bc bison flex libssl-dev libelf-dev cpio kmod \
        xz-utils curl ca-certificates python3 >/dev/null
    cd "$WORK"
    if [ ! -d "linux-$LINUX" ]; then
        curl -fsSL "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$LINUX.tar.xz" | tar xJ
    fi
    cd "linux-$LINUX"
    # virtio-gpu sends the whole screen after every page flip, assuming the
    # host keeps a copy of each buffer. BoltOS keeps none: it reads the
    # buffer shown out of guest memory and compares it with its own screen,
    # so the damage from one frame to the next is all it needs.
    sed -i '/old_plane_state->fb != new_plane_state->fb/{N;/ignore_damage_clips = true;/d}' \
        drivers/gpu/drm/virtio/virtgpu_plane.c
    make -s defconfig
    # No platform's drivers: the machine is BoltOS's, made of virtio devices.
    for p in $(sed -n 's/^config \(ARCH_[A-Z0-9_]*\)$/\1/p' arch/arm64/Kconfig.platforms); do
        scripts/config -d "$p"
    done
    while read -r op name; do
        case "$op" in ''|\#*) continue ;; esac
        scripts/config "$op" "$name"
    done < "$HERE/kernel.config"
    make -s olddefconfig
    make -s -j"$(nproc)" Image
    cp arch/arm64/boot/Image "$OUT/Image"
    ls -l "$OUT/Image"
}

root() {
    apt-get install -y -q --no-install-recommends debootstrap squashfs-tools curl ca-certificates gnupg >/dev/null
    R=$WORK/root
    rm -rf "$R"
    debootstrap --variant=minbase --components=main,contrib --include=ca-certificates,curl,gnupg trixie "$R" http://deb.debian.org/debian
    mount --bind /proc "$R/proc"; mount --bind /sys "$R/sys"; mount --bind /dev "$R/dev"
    trap 'umount -l "$R/proc" "$R/sys" "$R/dev" 2>/dev/null || true' EXIT
    install -d -m 0755 "$R/etc/apt/keyrings"
    curl -fsSL https://packages.mozilla.org/apt/repo-signing-key.gpg -o "$R/etc/apt/keyrings/packages.mozilla.org.asc"
    echo "deb [signed-by=/etc/apt/keyrings/packages.mozilla.org.asc] https://packages.mozilla.org/apt mozilla main" \
        > "$R/etc/apt/sources.list.d/mozilla.list"
    printf 'Package: *\nPin: origin packages.mozilla.org\nPin-Priority: 1000\n' > "$R/etc/apt/preferences.d/mozilla"
    chroot "$R" apt-get update -q
    chroot "$R" apt-get install -y -q --no-install-recommends \
        firefox cage seatd libavcodec61 ca-certificates iproute2 e2fsprogs util-linux procps cabextract curl \
        fontconfig fonts-liberation fonts-crosextra-carlito fonts-crosextra-caladea fonts-cascadia-code \
        fonts-noto-color-emoji fonts-wqy-microhei xcursor-themes adwaita-icon-theme-legacy unzip udev
    # Selawik: Microsoft's open stand-in for Segoe UI
    mkdir -p "$R/usr/local/share/fonts/selawik"
    curl -fsSL -o /tmp/selawik.zip https://github.com/microsoft/Selawik/releases/download/1.01/Selawik_Release.zip
    unzip -q -o -j /tmp/selawik.zip '*.ttf' -d "$R/usr/local/share/fonts/selawik"
    install -m 644 "$HERE/fonts.conf" "$R/etc/fonts/local.conf"
    chroot "$R" fc-cache -f
    chroot "$R" useradd -m -u 1000 -G video,input -s /bin/sh firefox || true
    install -m 755 "$HERE/init" "$R/sbin/boltinit"
    install -m 755 "$HERE/msfonts" "$R/usr/local/bin/boltos-msfonts"
    install -d "$R/usr/lib/firefox/defaults/pref" "$R/opt/boltfx"
    install -m 644 "$HERE/user.js" "$R/opt/boltfx/user.js"
    install -m 644 "$HERE/userChrome.css" "$R/opt/boltfx/userChrome.css"
    chroot "$R" firefox --version > "$R/opt/boltfx/version" 2>/dev/null || true
    cp "$R/opt/boltfx/version" "$OUT/version"
    # What a read-only root needs to be somewhere else, and what nobody reads.
    rm -rf "$R"/var/cache/apt/* "$R"/var/lib/apt/lists/* "$R"/usr/share/doc/* "$R"/usr/share/man/* \
           "$R"/usr/share/locale/[!e]* "$R"/usr/share/info/*
    umount -l "$R/proc" "$R/sys" "$R/dev"
    trap - EXIT
    rm -f "$OUT/root.sqfs"
    mksquashfs "$R" "$OUT/root.sqfs" -comp zstd -Xcompression-level 19 -b 256K -noappend -quiet
    ls -l "$OUT/root.sqfs"
}

case "${1:-all}" in
kernel) kernel ;;
root)   root ;;
all)    kernel; root ;;
esac
