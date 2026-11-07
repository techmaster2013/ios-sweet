#!/usr/bin/env python3
"""Apple Device Tree for the SWEET xnu built with -DSWEET_BOARD_PI5=1 (Raspberry Pi 5).

Same shape as tools/mkadt.py (the phone), with the Pi 5 values measured by
shim-pi5 stage 1 on real hardware (PI5-PORT.md, "Pi 5 hardware facts").
Usage:  mkadt_pi5.py OUT.adt [--qemu] [--dump]

--qemu: QEMU virt with -M virt,gic-version=2,virtualization=on -cpu cortex-a76
-smp 4: GICv2, PL011 and RAM at 0x40000000 like the Pi, other addresses differ.
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402
from mkadt import node, u32, u64, cstr, PANIC_LOG_SIZE  # noqa: E402

# Kernel-managed RAM. Pi 5 8 GB: 0x0-0x3f400000, then contiguous
# 0x40000000-0x200000000. Start with the 3 GB below 4G (the phone port's
# drivers assume 32-bit DMA); the early-console framebuffer block
# (0x3e000000-0x40000000) and the firmware's low memory stay outside.
PHYS_BASE = 0x40000000
MEM_SIZE = 0xc0000000
# QEMU virt also starts RAM at 0x40000000, but loads the shim and the payload
# (-kernel/-initrd) there: give the kernel the 3 GB from 4 GB up (-m 8G).
QEMU_PHYS_BASE = 0x100000000

PI5 = dict(
    soc_base=0x1000000000,        # /arm-io children are relative to this
    soc_size=0x1000000000,        # covers 0x10_0000_0000-0x1f_ffff_ffff (soc + RP1 window)
    gicd=(0x107fff9000, 0x1000),
    gicc=(0x107fffa000, 0x2000),
    uart=(0x107d001000, 0x200),   # PL011 on the 3-pin debug header
    timebase=54000000,
    vtimer_intid=27,              # PPI 11
    fb=(0x3f400000, 1920, 1080, 7680),
)
QEMU = dict(
    soc_base=0x01000000,
    soc_size=0x100000000 - 0x01000000,
    gicd=(0x08000000, 0x10000),
    gicc=(0x08010000, 0x10000),
    uart=(0x09000000, 0x1000),
    timebase=62500000,
    vtimer_intid=27,
    fb=None,                      # QEMU virt has no framebuffer: UART only
)

PI5_ECID = 0x5049350000000001     # 'PI5' + 1, synthetic
BOOT_MANIFEST_HASH = hashlib.sha384(b'pi5 boot manifest').digest()


def build(qemu=False, seed=None):
    hw = QEMU if qemu else PI5
    soc = hw['soc_base']
    rel = lambda addr: addr - soc
    seed = seed if seed is not None else os.urandom(64)
    phys_base, mem_size = (QEMU_PHYS_BASE if qemu else PHYS_BASE), MEM_SIZE
    panic_base = phys_base + mem_size - PANIC_LOG_SIZE
    mem_size -= PANIC_LOG_SIZE
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('embedded-panic-log-size', u32(PANIC_LOG_SIZE)),
        ('firmware-version', cstr('pi5-shim')),
        ('system-firmware-version', cstr('pi5-shim')),
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        ('crypto-hash-method', cstr('sha2-384')),
        ('display-scale', u32(2)),
        ('unique-chip-id', struct.pack('<Q', PI5_ECID)),
        ('chip-id', u32(0x8010)),     # A10, the iPad (6th generation) SoC
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # 4x Cortex-A76. reg = MPIDR Aff1:Aff0 as the hardware reports it, and
    # the PSCI CPU_ON target: Pi 5 0x000/0x100/0x200/0x300 (core in Aff1,
    # MT=1); QEMU virt 0x0/0x1/0x2/0x3 (core in Aff0; with 0x100 QEMU's PSCI
    # returns INVALID_PARAMETERS). XNU numbers CPUs in DT order.
    mpidr = (lambda core: core) if qemu else (lambda core: core << 8)
    cpu_nodes = []
    for core in range(4):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('arm,cortex-a76') + cstr('ARM,v8')),
            ('reg', u32(mpidr(core))),
            ('cpu-id', u32(core)),
            ('cluster-id', u32(0)),
            ('cluster-type', u32(0)),
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),
            ('l3-cache-id', u32(0)),
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(hw['timebase'])),
            ('clock-frequency', u32(2400000000)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(2133000000)),
            ('peripheral-frequency', u32(hw['timebase'])),
            ('fixed-frequency', u32(hw['timebase'])),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)), ('#size-cells', u32(0))], cpu_nodes)

    # pe_fiq.c (SWEET_GICV2): reg = <GICD size GICC size>, arm-io relative.
    gicd, gicd_size = hw['gicd']
    gicc, gicc_size = hw['gicc']
    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-400')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(hw['vtimer_intid'])),
        ('reg', u64(rel(gicd), gicd_size, rel(gicc), gicc_size)),
    ])
    # pe_serial.c VMAPPLE_UART (PL011) looks up the node named "uart0".
    uart, uart_size = hw['uart']
    uart0 = node('uart0', [
        ph(),
        ('compatible', cstr('arm,pl011')),
        ('reg', u64(rel(uart), uart_size)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('pi5-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, soc, hw['soc_size'])),
    ], [gic, uart0,
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        # Stage 4 decides the presented iPad size; the HDMI mode for now.
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(1920)),
            ('canvas-height', u32(1080)),
        ])])

    extra = []
    if hw['fb']:
        fb, w, h, stride = hw['fb']
        extra.append(node('sweet-fbcon', [
            ph(),
            ('reg', u64(fb, (stride * h + 0x3fff) & ~0x3fff)),
            ('width', u32(w)),
            ('height', u32(h)),
            ('stride', u32(stride)),
        ]))

    # bsd/dev/sweet_sd.c: the SD host the firmware booted from (rd=sdroot0).
    # reg = <SDHCI size brcmstb-cfg size>, absolute physical; QEMU virt has none.
    if not qemu:
        extra.append(node('sweet-sd', [
            ph(),
            ('compatible', cstr('brcm,bcm2712-sdhci')),
            ('reg', u64(0x107cfff000, 0x100, 0x107cfff400, 0x100)),
        ]))

    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])
    # iPad (6th generation) has a Home button: home-button-type 0.
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold') + cstr('menu')),
        ('home-button-type', u32(0)),
    ])
    pram = node('pram', [ph(), ('reg', u64(panic_base) + u64(PANIC_LOG_SIZE))])

    # Provisional iPad (6th generation, Wi-Fi) identity for the iPadOS 16
    # userland (stage 4 adds the real /product node from its IPSW).
    # 'raspberrypi,5-model-b' is what SweetPlatformExpert matches on Pi 5.
    return node('device-tree', [
        ph(),
        ('compatible', cstr('J71bAP') + cstr('iPad7,5') + cstr('raspberrypi,5-model-b')
         + cstr('brcm,bcm2712') + cstr('AppleARM')),
        ('model', cstr('iPad7,5')),
        ('target-type', cstr('J71b')),
        ('target-sub-type', cstr('J71bAP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, buttons, pram] + extra)


if __name__ == '__main__':
    root = build(qemu='--qemu' in sys.argv)
    blob = adt.serialize(root)
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
