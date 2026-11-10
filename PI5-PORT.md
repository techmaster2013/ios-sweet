# iPadOS 16 on Raspberry Pi 5 (8 GB): porting plan

Fork of ios-sweet. Goal: boot the iPadOS 16 userland on a Pi 5 with HDMI output
and a USB keyboard and mouse (landscape, external HID).

Status (2026-11-06): stage 1 shim in `shim-pi5/` builds cleanly with
gcc-aarch64-linux-gnu in WSL (`out/ios-pi5-shim.img`, 84 KiB, valid arm64
header, no relocations, no libc calls).

QEMU (2026-11-06, `shim-pi5/qemu.sh`: virt, gic-version=2, virtualization=on,
secure=off, 4x cortex-a76, 8 GB): **STAGE 1 PASSED, 0 failures.** Entered at
EL2, running at EL1; 16K granule yes, PAC no, GIC sysregs no; GICv2 vtimer
INTID 27 = 28 IRQs / 300 ms; PSCI 1.1 over smc; cpu1-3 started by CPU_ON,
each also entered at EL2 and dropped to EL1. QEMU does not cover: the HDMI
simple-framebuffer, the real Pi firmware's entry state and UART setup, or
GIC-400 security extensions (QEMU ran with secure=off).

**Real Pi 5 8 GB (2026-11-07): STAGE 1 PASSED, 0 checks failed** (user report;
shim booted as `kernel8.img` from the Pi OS Lite bootfs; values read from a
photo of the HDMI report). Next: stage 2 (XNU board config + GICv2 driver).

## Stage 2 design (2026-11-07)

- Build stays `MACHINE_CONFIG=SWEET`: ~100 `ARM64_BOARD_CONFIG_SWEET` gates,
  `MASTER.arm64.sweet` and the 3 makefile checks (0004, 0019, 0020) then apply
  unchanged. The Pi 5 is selected by one extra compile flag,
  `-DSWEET_BOARD_PI5=1 -mcpu=cortex-a76`, added to `MACHINE_FLAGS_ARM64_SWEET`
  by a new workflow input `board: sweet | pi5`.
- Pi 5 changes go in new patches numbered from 0101 (clear of upstream's
  0001-0077 series) and are gated `#if SWEET_BOARD_PI5`. Phone code keeps
  building as before when the flag is off.
- Needed in 0101+: SWEET.h board block without `HAS_GIC_V3` (VTIMER INTID 27,
  MAX_CPUS 4); GICv2 init in `pe_init_fiq` (GICD/GICC MMIO from the ADT);
  `sleh_irq` acking via GICC_IAR/EOIR MMIO; IPIs via GICD_SGIR with the target's
  GIC CPU-interface mask (read from banked GICD_ITARGETSR0 on each core);
  cpu number from MPIDR Aff1; drop the sweet drivers (UFS, SPMI, RPMh, keys,
  touch, battery, USB) from the PI5 build; framebuffer from the ADT.
- Writing patch hunks needs the patched xnu tree (xnu-8796.141.3 + blacktop's
  darwin-xnu-build patches + 0001-0081) locally, or the CI `xnu-buildtree`
  artifact from a fork.

### Stage 2 status (2026-11-07)

Done, not yet compiled (needs the macOS CI build):
- `xnu/patches/0101-pi5-board-gicv2.patch` (10 files, +226/-14), applies on
  the local tree = xnu-8796.141.3 + dxb seds + all patches as CI applies them
  (83 applied; skipped as expected: 0031/0041 need the staged HFS sources,
  blacktop's 4 version-specific patches). Contents:
  - `SWEET.h`: `SWEET_BOARD_PI5` (default 0), `SWEET_GICV2`, 4 CPUs,
    `VMAPPLE_UART` (PL011 driver), vtimer INTID 27, GICv2 register offsets,
    early framebuffer as constants (`SWEET_EARLY_FB_*`, block base + offset).
  - `pe_fiq.c`: GICv2 `pe_init_fiq` (GICD/GICC from ADT, banked
    ITARGETSR0 -> per-CPU interface mask, priorities 0xA0, PMR 0xF0),
    `sweet_gic_ack/eoi/send_sgi`.
  - `sleh.c`: IRQ ack/EOI through GICC MMIO on Pi 5. `SweetCPU.cpp`: IPIs via
    GICD_SGIR. `start.s`: no ICC_SRE_EL2 write on Pi 5 (UNDEFINED without GIC
    sysregs; secondaries enter at EL2), framebuffer constants.
  - `arm_vm_init.c`, `sweet_early.c`: framebuffer constants.
  - `pe_serial.c`: PL011 keeps the firmware's 115200 divisor on Pi 5.
  - `KernelConfigTables.cpp`: SweetPlatformExpert also matches
    `raspberrypi,5-model-b`, SweetCPU also `arm,cortex-a76`.
  - Phone drivers (UFS, SPMI keys, touch, USB) only start with
    `rd=ufsroot0`, so they stay off on Pi 5 with a RAM-disk root.
- `.github/workflows/xnu.yml`: input `board` (sweet | pi5). pi5 appends
  `-DSWEET_BOARD_PI5=1` to the SWEET MACHINE_FLAGS line inside 0004 (checked
  locally: the edited 0004 still applies). MACHINE_FLAGS feed both CFLAGS and
  SFLAGS, so assembly sees the define too.
- `tools/mkadt_pi5.py [--qemu]`: ADT with the measured Pi 5 values (RAM
  0x40000000 + 3 GB, arm-io at 0x10_0000_0000, GIC-400, uart0, 4 cpus with
  reg 0x000-0x300, sweet-fbcon 0x3f400000 1920x1080, provisional iPad7,5 /
  J71bAP identity). `--qemu`: QEMU virt GICv2/PL011 addresses, kernel RAM at
  4 GB (QEMU loads shim + initrd at 0x40000000).
- `shim-pi5/loader.c`: boots a SWEETXNU payload from the initramfs (Pi:
  `initramfs payload.bin followkernel`; QEMU: `-initrd`). Verified in QEMU with
  a fake 1-segment payload: copied to 0x100000000, entered at EL1 with
  boot_args Revision 2 in x0 (printed "K2").

Known risks to check on the first real boot:
- The early-console framebuffer is mapped as a 32 MB block 0x3e000000-0x40000000,
  which also covers RAM 0x3e000000-0x3f400000 (write-combine alias). Nothing
  of ours lives there; the firmware's DTB is at 0x2e03a500.
- In QEMU virt that block is PCIe ECAM space; early-console writes go nowhere.
- Untested assumption: GIC-400 CPU interface n = core n (fallback when a CPU
  has not stored its ITARGETSR mask yet).

CI (2026-11-07): fork `techmaster2013/ios-sweet`, branch `pi5-port`
(commits efea060 Pi 5 work, 8115044 runner input). The fork listed 0
workflows until an empty commit was pushed to its `main` (3a897f6).
Run 42376275793 was cancelled before any step: GitHub no longer offers
macos-14 ("configuration 'macos-14-arm64-us-default' is not supported";
upstream's last green runs were 2026-10-18). xnu.yml now has
`runner` (default macos-15); Deps picks the oldest Xcode on the image and
the build needs an Xcode 15 iig. Run 42376477421 (board=pi5,
runner=macos-15) **succeeded** with Xcode 16.0 (16A242d): its iig did not
break this tree. Patch 0101 applied, `SWEET_BOARD_PI5=1` in the flags
(grep count 1), kernel.development.sweet 17188656 bytes, plain arm64 PIE,
sha256 0beca9de...fcfac, contains "SWEET GIC: GICv2" and the new match rules
(local copy: xnu/out-pi5-0101/).

First QEMU boot (out-pi5/qemu-0101.log, payload from
`mkadt_pi5.py --qemu` + mkpayload, cmdline `debug=0xc serial=3 cpus=1
keepsyms=1 -enable_kprintf_spam -noprogress -v`): shim found the payload
(10 segs, phys 0x100000000, entry 0xfffffe000748c488), jumped at EL1;
xnu faulted at once in start.s stage 0: ESR 0x96000050 (external abort,
write), FAR 0x3fbad000 = the early-console marker band at
0x3f400000 + (1080-32)*7680, PCIe ECAM in QEMU virt. Fix (commit c886dcd,
in 0101): on Pi 5, start.s stage 0 and the sweet_early_* console run only if
boot_args->Video.v_baseAddr == SWEET_EARLY_FB_PA (the shim reports 0 in
QEMU). Also protects a real Pi whose HDMI mode moves the framebuffer.

**Run 42377501437 (c886dcd), QEMU cpus=1: XNU boots to the root-device
wait** (out-pi5/qemu-c886dcd-cpus1.log): "SWEET GIC: GICv2 GICD 0x8000000
GICC 0x8010000, virtual timer INTID 27", "cpu 0 interface mask 0x1",
Darwin Kernel Version 22.6.0 (xnu-8796.141.3), SweetCPU "SGI1 OK" (self-IPI
through GICD_SGIR + GICC ack/EOI), timer and scheduler running (Skywalk self
tests, ifnet threads), ends at `Waiting on <dict> IOProviderClass IOMedia,
Content Apple_HFS` = no root yet (stage 3).
cpus=4 failed twice, both QEMU-layout issues: PSCI CPU_ON rc=-2 for
target 0x100 (QEMU virt MPIDRs are 0x1-0x3), then SweetCPU's phone topology
check rejected mpidr 0x1. Fixed in 23d34a5 (mkadt_pi5 --qemu uses Aff0;
SweetCPU accepts 0x000-0x300 or 0x0-0x3 on Pi 5).

**Run 42378624537 (23d34a5), QEMU cpus=4: STAGE 2 PASSES IN QEMU**
(out-pi5/qemu-23d34a5-cpus4.log). All four CPUs: PSCI CPU_ON rc=0, enter at
EL2, drop to EL1, GIC interface masks 0x1/0x2/0x4/0x8, "SGI1 OK" each,
"4/4 CPUs initialized, SGI1 verified"; boot continues to the root-device wait.
The first cpus=4 run panicked "Interrupts held disabled for 21733680 ns
(timeout 13631488 ns)": the SCHED_HYGIENE interrupt-masked timeout is
0xd0000 timebase ticks, sized for Apple's 24 MHz timebase (35.5 ms); at
QEMU's 62.5 MHz it is 13.6 ms, at the Pi's 54 MHz 15.8 ms, and TCG emulation
of 4 CPUs is slow. Bring-up boot-args now include
`interrupt-masked-debug-mode=1` (TRACE: recorded in ktrace, not on the
console, so the serial log shows nothing either way). Revisit on hardware:
a ml-timeout-interrupt-masked override scaled to 54 MHz would keep PANIC mode.

Real-Pi payload ready: out-pi5/payload.bin (16590368 bytes, sha256 85c67008...,
kernel 23d34a5 + `mkadt_pi5.py` Pi ADT, cmdline `debug=0xc serial=3 cpus=4
keepsyms=1 -enable_kprintf_spam -noprogress -v interrupt-masked-debug-mode=1`).
Needs `initramfs payload.bin followkernel` in config.txt (shim-pi5/README.md).
SD card fixed 2026-11-07: bootfs (disk 1 partition 1) backed up to
`../sdcard-backup/bootfs` (434 files, hash-verified), quick-formatted FAT32
(now Healthy, creates files again), restored (hash-verified), plus
ios-pi5-shim.img and payload.bin; config.txt block "ios-pi5 stage 2" =
`kernel=ios-pi5-shim.img` + `initramfs payload.bin followkernel`.

**First real-Pi xnu boot (kernel 23d34a5, photo 2026-11-07):** shim found
the payload at 0x2e02d000-0x2efff620 (10 segs, phys 0x40000000 + 0xbff00000),
ADT 0x424c4000, boot_args 0x424c8000, jumped to 0x4148c488. xnu's early
console on HDMI printed SWEET EARLY 1-8 (arm_init, PE_init_platform,
ml_parse_cpu_topology, cpu_bootstrap, kernel_startup_bootstrap, cpu_init,
entering arm_vm_init, leaving bootstrap TTBR0); the stage bar ends at
segment 8. Stage 9 is the first draw through the final kernel mapping of the
framebuffer (sweet_early_relocate), a path QEMU never ran (no framebuffer,
console off). Suspected: that mapping, or the fault it causes with no
console left to report it. Commit b637d1d: Pi 5 early console from
boot_args->Video at runtime (start.s, arm_vm_init, sweet_early, arm_init),
QEMU fake framebuffer at 0x7e000000 from the shim, `FBDUMP=x.png qemu.sh`
to see it. Run 42385393260: QEMU draws stages 1-20 with the fake fb, also at
0x7f400000 (= the Pi's 0x1400000 offset inside its 32 MB block).

**Second real-Pi boot (8ffdc02, adds the 8b MMU check):** stops after
`SWEET EARLY 8b: fb VA 0xfffffe0003400000 PAR 0x000000000000000d want PA
0x000000003f400000`: PAR.F=1, FST 0b000110 = level 2 translation fault right
after the TTBR1 switch, although sweet_early_map_final wrote the block entry.
QEMU (same code, same offset) translates: PAR 0x440000007f400b00.
Hardware-only: either the walker does not see that write (cache/attribute
of the bootstrap-aperture alias it went through) or the final L1 entry
points at another L2 table on this memory layout. Commit cf66a8a: on a
fault the early console pauses ("8c") instead of hanging; after stage 11 the
entry is rewritten through the final tables + TLB flush and re-checked
("11b" prints the L1 entry and the slot before/after, which tells the two
causes apart); still faulting = early console off, boot continues.
`-sweet_fb_defer` forces that path for QEMU. Run 42388647441 (cf66a8a):
with `-sweet_fb_defer` QEMU boots on 4 CPUs to the root-device wait, so the
pause/retry/resume path works.

**QEMU 4-CPU panic, not a kernel bug:** most multi-threaded-TCG boots of
cf66a8a (normal and deferred alike) panic soon after "4/4 CPUs" with
`Ticket spinlock[0xfffffe0007b90880] timeout` = `_pset0+0x100`
(tools/macho_sym.py), waiter called from thread_select; the panicking CPU
also could not stop the others (DebuggerXCall). Not a missed WFE wakeup
(wfe_timeout_init runs on every CPU: 1 us event stream) and not host CPU
starvation (WSL has 12 cores). Single-threaded TCG with
`-icount shift=auto,sleep=off` (guest time = instructions executed) boots
4 CPUs cleanly to the root wait: the lock "timeouts" are emulation timing.
Use `QEMU_EXTRA="-accel tcg,thread=single -icount shift=auto,sleep=off"
qemu.sh` for SMP checks. Bring-up boot-args no longer have
`-enable_kprintf_spam`: kprintf is synchronous on the 115200 baud UART and
some of that output comes from inside the scheduler.

Pi payload (cf66a8a, cmdline `debug=0xc serial=3 cpus=4 keepsyms=1
-noprogress -v interrupt-masked-debug-mode=1`): out-pi5/payload.bin,
16607168 bytes, sha256 be5e82db...

**Third real-Pi boot (cf66a8a, 2026-11-07, user report): far past stage 8.**
- cpus=4: "4/4 CPUs" seen, battery 100% (SweetBattery stand-in), then the
  last line stays `ifnet_attach: All kernel threads created for interface
  lo0 have been scheduled at least once. Proceeding.` No gif0 "Waiting for
  all kernel threads" line, so it hangs between lo0's attach and gif0's.
- **cpus=1 (out-pi5/payload-cpus1.bin, sha256 e23a4677...): passes stf0 and
  stops at `Waiting on <dict> IOProviderClass IOMedia, Content Apple_HFS`,
  then "Failed to write boot-picker-bringup-reason to NVRAM" (no Apple
  NVRAM; harmless, also in QEMU). STAGE 2 REACHED ON REAL HARDWARE (1 CPU).**
- So the 4-CPU hang is SMP-only on the real GIC-400. Suspect: cross-CPU
  SGIs (only self-SGIs were checked on hardware; QEMU's GIC has no security
  extensions). Commit b2646a6 adds a `SWEET IPI: cpu A -> cpu B: OK|LOST`
  test after "4/4" and per-CPU IGROUPR0/GICC_CTLR/GICD_CTLR in the "SWEET
  GIC: cpu N" lines. Run 42398055028 building.

**STAGE 2 DONE ON REAL HARDWARE, 4 CPUs (b2646a6, photo 2026-11-07):**
"SWEET IPI: cpu 2 -> cpu 1/2/3: OK", then lo0, gif0 and stf0 attach,
`Waiting on <dict> IOMedia Apple_HFS`, "Failed to write
boot-picker-bringup-reason to NVRAM", "mcache: 4 CPU(s)". Lesson: the
sweet-fbcon console wraps to the top instead of scrolling (the newest line
is the one above the blank row, not the bottom one), so the earlier
"hangs after lo0" report for cf66a8a was probably a wrapped screen, not a
hang. Commit 8f4b278 (per-CPU timer check, run 42401147347) was started
before this result and is no longer needed for stage 2.

Next: stage 3 (root file system). iPad (6th generation, Wi-Fi) iPad7,5
iPadOS 16.7.x IPSW downloaded by the user; SDHCI driver for the BCM2712 SD
slot; HFS+ root partition in the unused space of the 64 GB card;
tools/mkiosroot.sh in WSL. IPSW: iPadOS 16.6.1 (20G81), the newest 16.x
for iPad7,5 (it went to 17.x after that; 17 does not match xnu-8796),
iPad_64bit_TouchID_ASTC_16.6.1_20G81_Restore.ipsw, 5727423701 bytes.

Next: `gh workflow run xnu.yml -R techmaster2013/ios-sweet --ref pi5-port -f board=pi5`, then
`mkpayload.py KERNEL out-pi5/pi5-qemu.adt payload.bin --cmdline "debug=0x8 serial=3 cpus=1 -v"`
and `shim-pi5/qemu.sh 60 payload.bin`.

## Pi 5 hardware facts (measured by the stage 1 shim, 2026-11-07)

- CPU: MIDR 0x414fd0b1 (Cortex-A76 r4p1). MPIDR cpu0 0x81000000, cpu1-3
  0x81000100/200/300: **MT=1, core number in Aff1, Aff0 = 0**. XNU's cpu/cluster
  numbering assumes Apple's layout (Aff0 = core, Aff1 = cluster), so the ADT
  cpu nodes and `ml_get_cpu_number`-style code must map Aff1 to the cpu number.
- MMFR0 0x101122 (16K + 4K granule, 40-bit PA), PFR0 0x1100000010111112 (GIC
  sysregs field = 0), ISAR1 0x100001 (no PAC).
- Entry: firmware enters at EL2; PSCI CPU_ON also starts secondaries at EL2.
  PSCI 1.1 over `smc`, SMCCC 1.2 (0x10002). CPU_ON returns 0 for all three.
- Timer: CNTFRQ 54 MHz. PPIs phys 14, virt 11, hyp 10 (INTID 30 / 27 / 26).
  vtimer INTID 27 = 29 IRQs per 300 ms at EL1.
- GIC-400: GICD 0x107fff9000, GICC 0x107fffa000. GICD_TYPER 0xfc69
  (ITLinesNumber 9 = 320 INTIDs, 4 CPU interfaces, SecurityExtn 1),
  GICD_IIDR 0x0200143b, GICC_IIDR 0x0202143b. Non-secure Group 1 works
  with GICD_CTLR bit 0 + GICC_CTLR bit 0, PMR 0xf0, priority 0xa0.
- RAM (8180 MiB): 0x0-0x3f400000, 0x40000000-0x100000000,
  0x100000000-0x200000000. Gap 0x3f400000-0x40000000 = firmware/framebuffer.
- Framebuffer: 0x3f400000, 1920x1080, stride 7680, a8r8g8b8 (16K aligned).
- Debug UART: PL011 at 0x107d001000 (from DT stdout-path).
- DTB loaded at 0x2e03a500 by the firmware. Addresses below come from the
upstream `bcm2712.dtsi` and have to be checked against the live DT the Pi
firmware hands over (the shim already parses the DT in `shim/fdt.c`).

## What carries over and what does not

| Area | ios-sweet code | On Pi 5 |
|---|---|---|
| Build: plain arm64 (no PAC), 16K pages, MASTER config | 0001-0009, `config/MASTER.arm64.sweet` | **Keep.** Cortex-A76 supports the 16K granule and has no PAC, same as Kryo 470. Rename board to `PI5`. |
| Crypto, PRNG, pthread, HFS+, AMFI, entitlements, keystore, sandbox | 0013-0015, 0019-0027, 0032-0038, 0050 | **Keep as is.** SoC-neutral. |
| IOSurface, IOMobileFramebuffer, HID event services, watchdog, crash/log tooling | 0040-0058, 0064, 0066-0068 | **Keep.** Framebuffer needs base/size/stride from DT instead of the sweet panel. |
| Device identity | 0059 (iPhone X `D22AP`), 0061 (1125x2436) | **Replace** with an iPad identity and a landscape HDMI-friendly size. |
| Interrupt controller | GICv3 via system registers (0004, 0017, 0018, 0065) | **Rewrite.** BCM2712 has a **GIC-400 (GICv2)**: memory-mapped CPU interface (GICC), no ICC_* sysregs, no redistributors. Biggest kernel change. |
| SMP / PSCI | PSCI via `smc` (0065) | **Keep.** Pi 5 runs TF-A BL31, PSCI over `smc`. |
| Timer | arch timer, vtimer PPI from ADT (0017) | **Keep.** PPI number from DT. |
| Boot entry | shim booted by `fastboot boot`, enters at EL1 | **Change.** Pi firmware loads `kernel_2712.img` and enters at **EL2**: shim must set HCR_EL2/CNTHCTL_EL2 and `eret` to EL1. |
| Console | framebuffer + sweet serial | Debug UART on the 3-pin JST header (PL011 `uart10`, ~0x10_7d001000). Needs a USB-UART probe. |
| Storage | `sweet_ufs.c`, `shim/ufs.c` (Qualcomm UFS) | **Replace** with SD card (BCM2712 SDHCI, on the SoC, not RP1). Root = HFS+ partition on the SD card. NVMe later. |
| Keys, touch, PMIC, battery | `sweet_spmi.c`, `sweet_rpmh.c`, `sweet_keys.c`, `sweet_touch.c`, `SweetBattery.cpp` | **Drop.** Fake "on AC power". |
| USB | `sweet_usb.c` (gadget, CDC-ECM) | **Replace.** Pi 5 USB-A ports sit on **RP1 behind PCIe**: need brcmstb PCIe root complex, RP1, then xHCI host (DWC3 in host mode), then USB HID boot protocol into `SweetHID`. Second-biggest piece. |
| GPU | none (CPU rendering, `SWContext`) | **Same.** CPU rendering on 4x A76 should be faster than on the phone's A55s. |

## Which iPadOS

The userland must be plain arm64 (no arm64e), so it has to come from an A10/A11-era
iPad on iOS 16: **iPad (6th generation), iPad7,5 / iPad7,6, iPadOS 16.7.x**.
Same steps as `tools/mkiosroot.sh` for the iPhone X IPSW. You download the IPSW from
Apple yourself; nothing Apple-made goes in git (same rule as upstream). Apple's
license only allows running iPadOS on Apple hardware, so this stays a personal
experiment.

Display: present 2048x1536 (native 6th-gen size, 4:3) and letterbox on HDMI, or
present 1920x1080 and hope SpringBoard copes. Start with 2048x1536 scaled down.

## Stages (each ends in something you can see)

1. **Shim on the Pi.** `config.txt` loads the shim as `kernel_2712.img`. EL2 to EL1
   drop, PL011 UART output, read DT: memory, GIC, UART, simple-framebuffer.
   Done when: "PI5 SHIM" on the UART and a coloured screen on HDMI.
2. **Kernel to `ml_wait_max_cpus`.** New board config, GICv2 driver
   (`Pi5GIC`: GICD + GICC, IAR/EOIR in MMIO, timer PPI as IRQ), ADT from
   `tools/mkadt.py --pi5`. Ramdisk root first, as sweet did in 0021.
   Can be tested in QEMU first (`-M virt,gic-version=2 -cpu cortex-a76`).
   Done when: XNU banner on UART, 4 CPUs up through PSCI.
3. **Root from SD.** SDHCI driver replacing `sweet_ufs.c`, HFS+ root on SD.
   Done when: launchd starts from the SD card.
4. **SpringBoard on HDMI.** iPad identity, framebuffer from DT.
   Done when: iPad home screen on the monitor (4-minute boot is fine).
5. **Keyboard and mouse.** PCIe link to RP1, xHCI host, HID boot protocol,
   feed `SweetHID`. Done when: typing in Notes and a pointer on screen.
6. **Later:** Ethernet (RP1 Cadence GEM), NVMe root, audio over HDMI.

## What is needed

- Raspberry Pi 5 8 GB, SD card (32 GB+), USB-to-UART probe for the debug header
  (the official Raspberry Pi Debug Probe fits it), HDMI monitor, USB keyboard + mouse.
- A GitHub fork for the macOS CI kernel build (`.github/workflows/xnu.yml`).
- Linux (WSL2 is fine) for the shim cross-compile and building the root image.

## Stage 3 hardware facts (from bcm2712-rpi-5-b.dtb on the card, 2026-11-07)

- SD slot: `/soc@107c000000/mmc@fff000`, compatible `brcm,bcm2712-sdhci`
  `brcm,sdhci-brcmstb`; reg "host" 0xfff000 + 0x260, "cfg" 0xfff400 + 0x200
  inside soc@107c000000 = PA 0x107cfff000 / 0x107cfff400. Interrupt SPI
  0x111 (level). Clock `clk-emmc2` fixed 200 MHz. bus-width 4, UHS
  SDR50/DDR50/SDR104, card detect on aon_gpio5. The firmware leaves the card
  initialized (it loaded payload.bin from it), possibly in 1.8 V UHS
  signalling: a full re-init (CMD0...) must keep Host Control 2's 1.8 V bit
  as found, since the card stays at 1.8 V until power is cut.
- Wi-Fi: `/axi/mmc@1100000`, same `brcm,bcm2712-sdhci`, PA 0x1001100000 /
  0x1001100400, SPI 0x112, non-removable, child `wifi@1` (CYW43455 over
  SDIO). On the SoC, not behind PCIe/RP1: the SD driver's host code is
  reusable for Wi-Fi.
- Card partitions are MBR (bootfs FAT32 512 MB, rootfs ext4 2.5 GB, rest of
  the 64 GB unallocated). Plan: MBR partition 3 of type 0xAF (HFS+) for the
  iPadOS root, found by type like sweet_ufs finds its GPT partition by name.
- Driver shape: copy bsd/dev/sweet_ufs.c's interface (bdevsw/cdevsw,
  strategy with partition bounds checks, devfs nodes, root via rd=), with
  SDHCI PIO transfers first (DMA later).

### Handoff 2026-11-08 evening

- **Works on the Pi:** internet over iPhone USB tethering (en1 ifnet in
  sweet_xhci.c + stand-in IONetworkController/IOEthernetInterface in
  iokit/Kernel/arm/SweetNet.cpp; configd does DHCP/DNS; Safari loads pages),
  SD High Speed 50 MHz, power button, wheel, About identity.
- **Card now:** root = image v9 (booted, cleanly unmounted on the last
  shutdown: journal empty, so it can be patched in place with
  tools/blockdelta.py + tools/sd-delta-write.ps1); payload = kernel a6b6037
  (CI 42543469734: PSCI SYSTEM_OFF/RESET for Shut Down/Restart, timestamps on
  every on-card log line) + `xhciscroll=8`.
- **Apps (PiInfo, PiStrap, uicache reload) not on the home screen:** the boot
  job (launchd com.ipados-rpi5.uicache, waits 60 s) registers all three via
  LSApplicationWorkspace: LSApplicationProxy says type=System installed=1
  valid=1 launchProhibited=0; it then SIGTERMs SpringBoard, which restarts,
  and still shows no icons. Open question to the user: do Spotlight / App
  Library show them? Info.plist differences vs Apple apps: no DTPlatformName/
  DTSDK*, CFBundleIconFile instead of CFBundleIcons, no scene manifest; apps
  registered without a data container. Signed with tools/adhoc_sign.py.
- **Boot time:** the slow first logo is the fresh-volume first boot (every
  full rewrite); next boot with the timestamp kernel shows where time goes.
- **Tomorrow:** boot (check Spotlight/App Library for the apps, faster wheel,
  Shut Down powers off?), read the timestamped log (boot time); try
  `wsl --mount \\.\PHYSICALDRIVE1 --bare` (admin; may refuse the built-in SD
  reader) so Linux can edit the root partition directly.
- **Open:** iPad Storage crash ([MPMediaLibrary defaultMediaLibrary]
  databasePath nil), Photos (root album list empty: photo library never
  created), Metal (IOAcceleratorES + MetalPluginName/MetalPluginClassName,
  initWithAcceleratorPort:), Wi-Fi research, PiStrap real install.

### Plans: USB tethering, Metal (2026-11-08)

- **USB tethering to an iPhone** (network without a router cable): xHCI bulk
  transfers; select the iPhone configuration with the Apple USB Ethernet
  interface (class 0xff/0xfd/1, Linux `ipheth`); an ifnet driver for its
  Ethernet frames; pairing over usbmux/lockdown (host certificates, Trust
  prompt, TLS session) so the phone enables tethering; Personal Hotspot on.
- **Metal** (user: Metal first, then GL). Image facts: the Metal driver is
  userland `System/Library/Extensions/AGXMetalA10.bundle` (A10 only), loaded
  for a kernel GPU service naming it; OpenGL ES is
  `AppleMetalGLRenderer.bundle` on top of Metal (GL follows Metal);
  CoreAnimation's shaders are `QuartzCore.framework/default.metallib` (AIR =
  LLVM bitcode). Plan: kernel stub GPU service naming our own
  `SweetMetal.bundle`; SweetMetal = software Metal (buffers, textures, render
  passes, the subset CoreAnimation uses) with shaders compiled ahead of time
  from AIR to arm64 on the PC and the `air.*` intrinsics implemented in C;
  later V3D for the hot paths. backboardd today matches `AGXAccelerator`,
  finds none and renders on the CPU.

### Stage 5 done, image v3 (2026-11-08)

- USB keyboard + mouse work (bsd/dev/sweet_xhci.c). Firmware PCIe state with
  pciex4_reset=0: window 0 CPU 0x1c00000000 -> PCI 0x80000000, root port bus
  numbers 0 (set to 0/1/1), inbound BAR2 PCI 0 (moved to 0x10_0000_0000);
  RP1 BARs 0x80410000/0x80000000/0x80400000. Mouse wheel needs report
  protocol (boot reports are 3 bytes). Power button = GPIO 20 @0x107d508500.
- Shim patches the ADT per board: serial-number (firmware /serial-number),
  dram-size + /pram (2 GB boards), /sweet-fbcon + disp0 canvas. About shows
  "Raspberry Pi 5" / "RPI5" / the serial.
- Crash analysis with tools/dsc_sym.py (shared cache symbolizer; cache copied
  to WSL ~/pi5-build/dsc). Settings > iPad Storage: PodcastsUI force-unwraps
  [MPMediaLibrary defaultMediaLibrary].databasePath (nil: no
  /var/mobile/Media/iTunes_Control). Photos: assetsd uncaught ObjC exception
  (reason not yet logged).
- The card's root after a power cut has a non-empty journal (start 1799168 end
  3372544): Linux cannot edit it (no replay, fsck_hfs ignores the journal).
  So v3 = v2 + iTunes_Control/iTunes (501) + assetsd stdout/stderr to console
  + time zone America/New_York (localtime + zoneinfo links + datetime pref),
  SHA256 14af46d3...2b91d, written + verified 2026-11-08 11:31.

### LOCK SCREEN on the Pi (2026-11-07, ~22:30 local)

- Card: partition 3 = v2 image minus bluetoothd and fairplayd.A2 jobs
  (SHA256 b4dc709b...59c5, written + verified by sd-write-root.ps1), payload
  = out-pi5/payload-sdrw.bin (kernel 44026ab, cmdline above).
- Boot: `exec mount_hfs` sat ~10+ min (root remount; cause unknown, probably
  slow PIO reads), then mmaintenanced, `SWEET AKS: linkd/triald sel 17 -> 0`,
  many SpringBoard lines, logo fade, **iPadOS lock screen on HDMI**:
  landscape, correct proportions (only the boot logo was squashed), "Press
  home to open", battery 100% charging, date Wed Dec 31 (epoch 0, no RTC).
  Console text drawn over it (`SWEET FB: backboardd sel 72 ...`).
- Pending (not yet on the card):
  - kernel a009ff0 (CI 42441823788, built): SD console log to MBR
    partition 4 type 0xDA (64 MiB at byte 20258488320; created by
    scratchpad sd-log.ps1, which also reads it back -> tools/sweet-log.py).
  - kernel 5c503df (CI running): + boot-arg sweettime=<unix secs> seeds the
    calendar in PEGetUTCTimeOfDay.
  - next cmdline = previous + `sweetfb_noconsole=1 sweettime=$(date +%s)`.
- Next: card in PC -> sd-log.ps1 (UAC) creates partition 4 -> new payload;
  then stage 5 (USB HID) to unlock; SD speed (HS/DMA); network; Sileo.

### First userland boot on the Pi (2026-11-07, evening)

- Kernel 44026ab (CI 42421982536) + ADT with /sweet-sd. Read-only boot
  (`rd=sdroot0` only): `mount(7) failed` / `cannot mount root, errno = 19`
  (journaled HFS+ needing journal init cannot mount read-only).
- Payload `out-pi5/payload-sdrw.bin` (on the card as payload.bin), cmdline
  `debug=0xc serial=3 cpus=4 keepsyms=1 -noprogress -v rd=sdroot0 sdrw=1
  rootrw=1 unentitled_ios_sim_launch=1 ignition_level=1
  wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1`: **root mounts,
  launchd runs, daemons start**; an Apple logo + moving progress bar is drawn
  by userland on HDMI (squashed ~2.4x vertically: portrait-panel
  assumption). Seen: `exec xpcproxy` / `exec bluetoothd` repeating (no BT
  hardware), fairplayd repeating, aggregated.
- The image on the card was plain mkiosroot.sh output, WITHOUT
  tools/sweet-root-tweaks.py: the progress bar is DataMigration (runs ~1 h
  on every boot without com.apple.migration LastSystemVersion), Setup is not
  skipped, CommCenter not removed, _PanicOnCrash still set (7 jobs).
- `ipados-16.6.1-20G81-root-v2.img` (WSL ~/pi5-build) = image + tweaks with
  SWEET_BUILD=20G81 SWEET_VERSION=16.6.1, SHA256 eadf91ed...872f, fsck OK.
  Data ark skipped (no mobileactivationd container before first boot);
  activation comes from the /chosen security-status properties anyway.
  Not yet on the card (needs the elevated sd-write-root.ps1 again).
- Display TODO (stage 4): landscape 1920x1080 panel, display-scale 1.

### SD root driver (bsd/dev/sweet_sd.c, 2026-11-07)

- ADT `/sweet-sd`, reg = <0x107cfff000 0x260 0x107cfff400 0x200> (Pi only).
- Boot with `rd=sdroot0`. Boot-args: `sdpart=N` (pick MBR entry N instead
  of the first 0xAF), `sdrw=1` (allow writes, bounds-checked to the
  partition), `sdclk=` (Hz, capped at 25 MHz), `sdbase=` (override the
  SDHCI base clock if the caps field is wrong).
- Init: dump host registers, force card detect (brcmstb cfg CD test bits,
  then SDHCI host-control test bits) if not present, reset CMD/DAT only (not
  ALL, which would drop the 1.8 V bit), UHS mode -> SDR12, 400 kHz, CMD0,
  CMD8, ACMD41, CMD2/3/9/7, ACMD6 4-bit, CMD16, 25 MHz. Then MBR scan and
  the HFS+ signature at partition byte 1024, all printed as `SWEET SD:`.
- Polled PIO, CMD17/18 (auto CMD12) and CMD24/25, max 2048 blocks a command.
  At 25 MHz 4-bit that is ~12 MB/s at best; high speed / DMA come later.

## Stage 3 build environment (WSL2, 2026-11-07)

- IPSW iPadOS 16.6.1 (20G81) in the user's Downloads (5727423701 bytes, size
  matches ipsw.me). BuildManifest, identity j71bap: OS 096-56598-083.dmg,
  Cryptex1,SystemOS 096-57766-083.dmg, Cryptex1,AppOS 096-57613-084.dmg,
  DeviceTree Firmware/all_flash/DeviceTree.j71bap.im4p. Extracted to
  ~/pi5-build/ipsw in WSL (not in git).
- /sbin/launchd is plain arm64 (not arm64e): runs on the A76 without PAC.
- Root steps run with `wsl -u root` (user approved 2026-11-07; no sudo
  password involved).
- apfs-fuse (sgan81, built in ~/pi5-build/apfs-fuse/build) needs, with
  CMake 4 / GCC 15: -DCMAKE_POLICY_VERSION_MINIMUM=3.5, and `#include
  <cstdint>` + `<vector>` at the top of ApfsLib/PList.cpp.
- HFS+: WSL's kernel has no CONFIG_HFSPLUS_FS. tools/wsl-hfsplus.sh builds
  hfsplus.ko for the running kernel (6.18.58.1-microsoft-standard-WSL2) with
  gcc-13 + gcc-13-plugin-dev; a GCC 15 build is rejected ("disagrees about
  version of symbol module_layout"). Reload with `wsl-hfsplus.sh load` after
  each WSL restart. Verified: mkfs.hfsplus + loop mount + write + read back.
- **iPadOS root image built (2026-11-07):** ~/pi5-build/ipados-16.6.1-20G81-root.img
  in WSL (16 GiB sparse, not in git), `mkiosroot.sh` from the j71bap OS +
  SystemOS + AppOS images, ~3.5 min. HFSX ('HX' v5), journaled, cleanly
  unmounted, not inconsistent, 174262 files / 111889 folders, 10.9 GiB used.
  Checked read-only: launchd, dyld, SpringBoard, backboardd, shared cache
  hard-linked into /System/Library/Caches/com.apple.dyld, log stores 272:272,
  systembag.kb 0:0 0600, 478 LaunchDaemons. libSystem.B.dylib exists only in
  the shared cache (normal for iOS 16).
  - mkiosroot.sh fix: Ubuntu 26.04's uutils `install` rejects owners with no
    host passwd entry ("install: invalid user: '272'"); owners now set with
    chown (numeric IDs work everywhere).
  - `fsck.hfsplus -n` after the `-y` pass still says "Volume header needs
    minor repair (1, 0)" / "invalid primary VHB ... result 35" (VIStat
    0x8000 only, all B-tree/catalog checks clean) while `-y` says OK: a
    read-only-mode quirk of the Linux fsck port, header flags are fine.

### 2026-11-09: hidden apps + slow first logo found

- Apps: LaunchServices and SpringBoard knew them all along (App Library
  category cache lists PiInfo/PiStrap/UICacheReload, MCM data containers
  exist). SpringBoard never placed them: home page 1 is full of widgets, so
  they went to the App Library only. v10 = card + IconState.plist page 2 with
  the three apps (5-block delta, written + verified 15:16).
- Slow first Apple logo: 30 s gap in the kernel log (2.0 s -> 31.8 s) =
  IOKitInitializeTime waiting for an IORTC that never comes. Patch 0101 now
  skips it on ARM64_BOARD_CONFIG_SWEET (commit 7acde54, CI 42689603214).
  Rest of boot: launchd 32.7 s, SpringBoard 39.8 s.
- Shutdown: launchd teardown + unmount ran cleanly; the log ends before the
  PSCI line, so whether the board powered off needs the user's eyes.

### 2026-11-09 evening: apps visible, shutdown works, boot 30 s faster

- Hidden apps fixed: `SBShowNonDefaultSystemApps = true` in
  /var/mobile/Library/Preferences/com.apple.springboard.plist (v11 delta,
  9 blocks). FrontBoard's library had listed them all along. Apps appear.
- Shut Down powers the Pi off (PSCI SYSTEM_OFF) - confirmed by the user.
- Kernel 7acde54 (CI 42695193321, macos-15-intel runner: arm64 runners were
  out of capacity): root at 2.3 s, launchd 2.3 s, SpringBoard 9.3 s.
- Payload now xhciscroll=24 (8 still too slow); not yet judged.
- Card: root = v11 (card copy 754f6c56... + delta), payload fca708ca...

### 2026-11-09 night: queued for the next card trip

- Kernel 8c3361e (CI 42719715264): Wi-Fi step 1 (bsd/dev/sweet_wifi.c: WL_ON
  GPIO 28, SDIO host 0x1001100000, CMD5/3/7, function 1, chip ID; logs the
  pinctrl mux for pins 30..35, boot-arg wifimux=N to set it) + CPU clock log.
- Payload: sweetfb_hz=30 (was default 10 = the main reason the screen looks
  choppy), xhciscroll=24.
- Root v12 (make-v12.sh in scratchpad; needs a fresh card read first since the
  card was booted): uicache reload without respring (20 s wait), PiInfo with a
  Hardware row, PiStrap with the setuid-root helper + bundled packages
  (~29 MB), AssistiveTouch on (com.apple.Accessibility AssistiveTouchEnabled).
- If the CPU clock log is low: config.txt force_turbo=1 (firmware still throttles).

### 2026-11-09 late: CPU runs ~40x slow

- Kernel 185e70f (wifi=0) boot: `SWEET: cpu clock ~57 MHz (10M dependent adds
  in 173011 us)` at platform-expert start (1.8 s). A76 should do that in
  ~4-7 ms. SCTLR (C/I/M), HCR_EL2 (RW only) and MAIR look normal in start.s.
  Candidates: interrupt storm (timer PPI group oddity in the log) vs clock /
  cache state. Kernel 74629ee (CI 42738693649) probes with IRQ/FIQ open and
  masked, at boot and 30/120 s after root.
- Wi-Fi driver panics within ~2 s of root (before the first SD log flush).
  Kernel 2e79611 makes it opt-in (wifi=1) with STEP logging + 3 s sleeps.
- PiStrap ran on the Pi (exec pistrap-helper x3, then SpringBoard restarted
  at 229 s); result not checked yet.
- Smoother screen at sweetfb_hz=30 confirmed by the user.
