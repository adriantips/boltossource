#!/usr/bin/env bash
# ===========================================================================
#  BoltOS Web Engine  -  webengine/vm.sh
#
#  The engine is a real Firefox, set up to look like Firefox on Windows, in a
#  small Debian VM. BoltOS Mini's Browser connects to it on port 7310, sends
#  clicks and keys, and gets the page back as H.264 (webengine/boltweb.py).
#  This script makes and runs that VM with QEMU on this Mac: Apple's
#  hypervisor runs it on the M-series cores directly, so Firefox is as fast
#  here as it is on Linux.
#
#      bash webengine/vm.sh setup     download Debian, make the VM, install
#                                     Firefox, the fonts and the engine (once)
#      bash webengine/vm.sh start     run it in the background
#      bash webengine/vm.sh stop
#      bash webengine/vm.sh status    is it up, and the engine's key
#      bash webengine/vm.sh sync      copy webengine/ in again, restart the engine
#      bash webengine/vm.sh pair [PI] tell a Mini where the engine is, and its key
#      bash webengine/vm.sh logs      the engine's log, following
#      bash webengine/vm.sh ssh [cmd]
#
#  Everything it makes lives in build/webengine/ (gitignored): the Debian
#  image, the VM's disk, its SSH key and the engine's key.
# ===========================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HERE="$ROOT/webengine"
DIR="$ROOT/build/webengine"
QEMU="${QEMU:-$ROOT/build/.tools/qemu/bin/qemu-system-aarch64}"
QIMG="${QIMG:-$ROOT/build/.tools/qemu/bin/qemu-img}"
[ -x "$QEMU" ] || QEMU=qemu-system-aarch64
[ -x "$QIMG" ] || QIMG=qemu-img
SHARE="$(cd "$(dirname "$(command -v "$QEMU")")/../share/qemu" 2>/dev/null && pwd || echo /usr/share/qemu)"
BASE="$DIR/debian-13-arm64.qcow2"
DISK="$DIR/disk.qcow2"
PORT="${BOLTWEB_PORT:-7310}"
SSHP="${BOLTWEB_SSH:-2222}"
MEM="${BOLTWEB_MEM:-4G}"
CPUS="${BOLTWEB_CPUS:-4}"
SSH=(ssh -i "$DIR/id_ed25519" -p "$SSHP" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
     -o LogLevel=ERROR -o ConnectTimeout=5 boltweb@127.0.0.1)
mkdir -p "$DIR"

running() { [ -f "$DIR/qemu.pid" ] && kill -0 "$(cat "$DIR/qemu.pid")" 2>/dev/null; }

wait_ssh() {
    for _ in $(seq 1 120); do
        "${SSH[@]}" true 2>/dev/null && return 0
        sleep 2
    done
    echo "ERROR: the VM didn't come up (see $DIR/console.log)"; exit 1
}

start() {
    if running; then echo "already running (pid $(cat "$DIR/qemu.pid"))"; return; fi
    [ -f "$DISK" ] || { echo "ERROR: no VM yet -- run: bash webengine/vm.sh setup"; exit 1; }
    ACCEL=(-accel hvf -cpu host)
    [ "$(uname -s)" = Linux ] && ACCEL=(-accel kvm -cpu host)
    "$QEMU" -M virt "${ACCEL[@]}" -smp "$CPUS" -m "$MEM" \
        -drive if=pflash,format=raw,readonly=on,file="$SHARE/edk2-aarch64-code.fd" \
        -drive if=pflash,format=raw,file="$DIR/vars.fd" \
        -drive if=virtio,file="$DISK",format=qcow2 \
        -drive if=virtio,file="$DIR/seed.iso",format=raw,readonly=on \
        -netdev user,id=n0,hostfwd=tcp:127.0.0.1:"$SSHP"-:22,hostfwd=tcp::"$PORT"-:7310 \
        -device virtio-net-pci,netdev=n0 -device virtio-rng-pci \
        -display none -serial file:"$DIR/console.log" \
        -daemonize -pidfile "$DIR/qemu.pid"
    echo "VM starting (pid $(cat "$DIR/qemu.pid")); engine on port $PORT"
}

sync_files() {
    "${SSH[@]}" "sudo mkdir -p /opt/boltweb && sudo chown boltweb: /opt/boltweb"
    scp -q -i "$DIR/id_ed25519" -P "$SSHP" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
        "$HERE/boltweb.py" "$HERE/user.js" "$HERE/fonts.conf" "$HERE/boltweb.service" "$HERE/provision.sh" \
        boltweb@127.0.0.1:/opt/boltweb/
    "${SSH[@]}" "printf '%s' '$(cat "$DIR/key")' > /opt/boltweb/key"
}

case "${1:-}" in
setup)
    command -v "$QEMU" >/dev/null || [ -x "$QEMU" ] || { echo "ERROR: no qemu-system-aarch64"; exit 1; }
    if [ ! -f "$BASE" ]; then
        echo "== downloading Debian 13 (arm64 cloud image)"
        curl -fSL -o "$BASE.part" https://cloud.debian.org/images/cloud/trixie/latest/debian-13-genericcloud-arm64.qcow2
        curl -fsSL -o "$DIR/SHA512SUMS" https://cloud.debian.org/images/cloud/trixie/latest/SHA512SUMS
        want=$(grep "genericcloud-arm64.qcow2" "$DIR/SHA512SUMS" | awk '{print $1}')
        [ "$(shasum -a 512 "$BASE.part" | awk '{print $1}')" = "$want" ] || { echo "ERROR: checksum"; exit 1; }
        mv "$BASE.part" "$BASE"
    fi
    [ -f "$DIR/id_ed25519" ] || ssh-keygen -q -t ed25519 -N "" -C boltweb -f "$DIR/id_ed25519"
    [ -f "$DIR/key" ] || openssl rand -hex 8 > "$DIR/key"
    if [ ! -f "$DISK" ]; then
        "$QIMG" create -q -f qcow2 -b "$BASE" -F qcow2 "$DISK" 24G
        cp "$SHARE/edk2-arm-vars.fd" "$DIR/vars.fd"
        rm -rf "$DIR/seed" && mkdir -p "$DIR/seed"
        printf 'instance-id: boltweb-1\nlocal-hostname: boltweb\n' > "$DIR/seed/meta-data"
        cat > "$DIR/seed/user-data" <<EOF
#cloud-config
users:
  - name: boltweb
    shell: /bin/bash
    sudo: ALL=(ALL) NOPASSWD:ALL
    ssh_authorized_keys:
      - $(cat "$DIR/id_ed25519.pub")
growpart: { mode: auto, devices: ["/"] }
package_update: false
EOF
        rm -f "$DIR/seed.iso"
        if command -v hdiutil >/dev/null; then
            hdiutil makehybrid -quiet -iso -joliet -default-volume-name cidata -o "$DIR/seed.iso" "$DIR/seed"
        else
            genisoimage -quiet -o "$DIR/seed.iso" -V cidata -J -r "$DIR/seed"
        fi
    fi
    start
    echo "== waiting for the VM"
    wait_ssh
    sync_files
    echo "== installing Firefox, the fonts and the engine (a few minutes)"
    "${SSH[@]}" "sudo bash /opt/boltweb/provision.sh"
    echo "== done. The engine's key: $(cat "$DIR/key")"
    echo "   Point a Mini at it:  bash webengine/vm.sh pair"
    ;;
start)  start ;;
stop)
    if running; then
        "${SSH[@]}" "sudo poweroff" 2>/dev/null || true
        for _ in $(seq 1 30); do running || break; sleep 1; done
        running && kill "$(cat "$DIR/qemu.pid")"
        echo stopped
    else echo "not running"; fi
    ;;
status)
    if running; then
        echo "VM running (pid $(cat "$DIR/qemu.pid")), engine on port $PORT, key $(cat "$DIR/key")"
        "${SSH[@]}" "systemctl is-active boltweb; tail -3 /var/log/boltweb.log 2>/dev/null" || true
    else echo "not running"; fi
    ;;
sync)
    sync_files
    "${SSH[@]}" "sudo install -m 644 /opt/boltweb/boltweb.service /etc/systemd/system/ && sudo install -m 644 /opt/boltweb/fonts.conf /etc/fonts/local.conf && sudo systemctl daemon-reload && sudo systemctl restart boltweb"
    echo "engine restarted"
    ;;
pair)
    # The Mini's dev page takes the engine's address and key. Over USB-C the
    # Mac is 10.77.0.2 to the Pi; QEMU's Mini reaches the Mac at 10.0.2.2.
    PI="${2:-${BOLTOS_PI:-10.77.0.1}}"
    HOST="${3:-10.77.0.2}"
    case "$PI" in localhost*|127.*) HOST="${3:-10.0.2.2}" ;; esac
    curl -sS -g "http://$PI/engine?set=$HOST:$PORT&key=$(cat "$DIR/key")"
    ;;
logs)   "${SSH[@]}" "tail -n 60 -f /var/log/boltweb.log" ;;
ssh)    shift; "${SSH[@]}" "$@" ;;
*)      sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//' ; exit 1 ;;
esac
