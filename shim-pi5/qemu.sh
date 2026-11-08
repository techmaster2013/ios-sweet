#!/bin/bash
# Smoke-test the shim in QEMU with a Pi 5-like setup: 4x Cortex-A76, 8 GB,
# GICv2, entry at EL2 (virtualization=on), PSCI over smc. Output is the PL011
# on stdout. QEMU virt has no framebuffer: the shim reports a fake 1920x1080
# one at 0x7f400000 (guest RAM, same in-block offset as the Pi) so xnu's early console runs as on a Pi.
#
# Usage: [FBDUMP=out.png] qemu.sh [SECONDS] [PAYLOAD]
#   no payload: stage 1 checks
#   PAYLOAD (tools/mkpayload.py with an ADT from tools/mkadt_pi5.py --qemu): boots xnu
#   FBDUMP: save the fake framebuffer as a PNG just before QEMU is stopped
set -euo pipefail
cd "$(dirname "$0")"
secs=${1:-6}
payload=${2:-}
args=(-M virt,gic-version=2,virtualization=on,secure=off
	-cpu cortex-a76 -smp 4 -m 8G
	-nographic -no-reboot -nic none
	-kernel out/ios-pi5-shim.img)
[ -n "$payload" ] && args+=(-initrd "$payload")
# Extra QEMU options, e.g. QEMU_EXTRA="-accel tcg,thread=single -icount shift=auto,sleep=off"
# (guest time follows executed instructions: no host-timing lock timeouts).
# shellcheck disable=SC2206
[ -n "${QEMU_EXTRA:-}" ] && args+=($QEMU_EXTRA)

if [ -z "${FBDUMP:-}" ]; then
	timeout --foreground "$secs" qemu-system-aarch64 "${args[@]}" || true
	exit 0
fi

sock=$(mktemp -u /tmp/qemu-mon.XXXXXX)
raw=$(mktemp /tmp/qemu-fb.XXXXXX)
qemu-system-aarch64 "${args[@]}" -monitor "unix:$sock,server,nowait" &
pid=$!
sleep "$secs"
python3 - "$sock" "$raw" <<'EOF'
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
time.sleep(0.3)
s.sendall(f'pmemsave 0x7f400000 {1920 * 1080 * 4} "{sys.argv[2]}"\n'.encode())
time.sleep(2)
s.sendall(b'quit\n')
time.sleep(0.5)
EOF
wait "$pid" 2>/dev/null || true
python3 ../tools/fb2png.py "$raw" "$FBDUMP" 1920 1080
rm -f "$raw" "$sock"
