#!/usr/bin/env bash
# ===========================================================================
#  BoltOS Mini  -  mini/firefox/make-image.sh
#
#  Builds Mini's Firefox machine (the guest kernel and root filesystem,
#  mini/firefox/build.sh) in the arm64 Debian VM that webengine/vm.sh keeps
#  on this Mac, and brings the results back to build/mini/firefox/. The VM
#  only builds; the Pi runs what it makes on its own.
#
#      bash mini/firefox/make-image.sh [kernel|root|all]
# ===========================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VM="$ROOT/webengine/vm.sh"
DIR="$ROOT/build/webengine"
OUT="$ROOT/build/mini/firefox"
mkdir -p "$OUT"
[ -f "$DIR/disk.qcow2" ] || bash "$VM" setup
bash "$VM" start >/dev/null || true
for _ in $(seq 1 90); do bash "$VM" ssh true 2>/dev/null && break; sleep 2; done
SCP=(scp -q -i "$DIR/id_ed25519" -P "${BOLTWEB_SSH:-2222}" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)
bash "$VM" ssh "sudo rm -rf /opt/boltfx/src && sudo mkdir -p /opt/boltfx/src && sudo chown boltweb: /opt/boltfx/src"
"${SCP[@]}" "$ROOT"/mini/firefox/* boltweb@127.0.0.1:/opt/boltfx/src/
bash "$VM" ssh "sudo bash /opt/boltfx/src/build.sh ${1:-all}"
for f in Image root.sqfs version; do
    bash "$VM" ssh "test -f /opt/boltfx/out/$f" && "${SCP[@]}" "boltweb@127.0.0.1:/opt/boltfx/out/$f" "$OUT/$f"
done
ls -l "$OUT"
