#!/usr/bin/env bash
# ===========================================================================
#  Shared QEMU discovery for run.sh / run-uefi.sh / test/run-tests.sh.
#  Sourced, not executed. Exports:
#     QEMU      path to qemu-system-x86_64
#     OVMF_CODE path to the UEFI firmware image    (may be empty)
#     OVMF_VARS path to its writable vars template (may be empty)
#     ROOT      repo root, already translated to a Windows path under WSL
#  Override any of them from the environment (BOLTOS_QEMU / BOLTOS_OVMF_CODE).
# ===========================================================================

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

QEMU="${BOLTOS_QEMU-}"
if [ -z "$QEMU" ]; then
    for c in "/c/Program Files/qemu/qemu-system-x86_64.exe" \
             "/mnt/c/Program Files/qemu/qemu-system-x86_64.exe" \
             /c/msys64/ucrt64/bin/qemu-system-x86_64.exe \
             /mnt/c/msys64/ucrt64/bin/qemu-system-x86_64.exe; do
        [ -f "$c" ] && { QEMU="$c"; break; }
    done
fi
if [ -z "$QEMU" ] && command -v qemu-system-x86_64 >/dev/null 2>&1; then
    QEMU="$(command -v qemu-system-x86_64)"
fi
[ -n "$QEMU" ] || { echo "ERROR: QEMU not found (set BOLTOS_QEMU)"; exit 1; }

# UEFI firmware ships beside QEMU as edk2-x86_64-code.fd (msys2/Arch naming) or
# as OVMF_CODE.fd (Debian/Fedora). The vars template is looked up alongside it.
OVMF_CODE="${BOLTOS_OVMF_CODE-}"
OVMF_VARS="${BOLTOS_OVMF_VARS-}"
if [ -z "$OVMF_CODE" ]; then
    for d in "$(dirname "$QEMU")/../share/qemu" "$(dirname "$QEMU")/share" \
             /usr/share/OVMF /usr/share/ovmf /usr/share/edk2/x64; do
        for f in edk2-x86_64-code.fd OVMF_CODE.fd OVMF_CODE.4m.fd; do
            [ -f "$d/$f" ] && { OVMF_CODE="$d/$f"; break 2; }
        done
    done
fi
if [ -n "$OVMF_CODE" ] && [ -z "$OVMF_VARS" ]; then
    _d="$(dirname "$OVMF_CODE")"
    for f in edk2-i386-vars.fd OVMF_VARS.fd OVMF_VARS.4m.fd; do
        [ -f "$_d/$f" ] && { OVMF_VARS="$_d/$f"; break; }
    done
fi

# QEMU is a Windows executable even under WSL, so hand it Windows paths.
command -v wslpath >/dev/null 2>&1 && ROOT=$(wslpath -w "$ROOT")
