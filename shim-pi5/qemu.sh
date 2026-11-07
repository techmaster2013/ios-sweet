#!/bin/bash
# Smoke-test the stage 1 shim in QEMU with a Pi 5-like setup: 4x Cortex-A76,
# 8 GB, GICv2, entry at EL2 (virtualization=on), PSCI over smc.
# No framebuffer in QEMU virt: output is the PL011 on stdout.
set -euo pipefail
cd "$(dirname "$0")"
# Usage: qemu.sh [SECONDS] [PAYLOAD]
#   no payload: stage 1 checks
#   PAYLOAD (tools/mkpayload.py with an ADT from tools/mkadt_pi5.py --qemu): boots xnu
secs=${1:-6}
payload=${2:-}
timeout --foreground "$secs" qemu-system-aarch64 \
	-M virt,gic-version=2,virtualization=on,secure=off \
	-cpu cortex-a76 -smp 4 -m 8G \
	-nographic -no-reboot -nic none \
	-kernel out/ios-pi5-shim.img ${payload:+-initrd "$payload"} || true
