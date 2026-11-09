# ipados-rpi5

[![xnu](https://github.com/techmaster2013/ipados-rpi5/actions/workflows/xnu.yml/badge.svg)](https://github.com/techmaster2013/ipados-rpi5/actions/workflows/xnu.yml)

**Apple's iPadOS 16 running on a Raspberry Pi 5.**

This project boots Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) on a Raspberry Pi 5, with the **unmodified iPadOS 16.6.1 (20G81) userland of the iPad (6th generation)** on top: launchd, the system daemons, backboardd, SpringBoard and the stock apps. It is built on [ios-sweet](https://github.com/dipa-studio/ios-sweet), which did the same for the Redmi Note 10 Pro; that project's README is kept in [docs/README-sweet.md](docs/README-sweet.md).

As of **2026-11-08** the Pi boots to the **iPadOS lock screen and home screen on HDMI**, in landscape. A USB keyboard and mouse work: you can unlock it, open apps (Settings, Messages, Safari, the App Library) and type, and with an iPhone plugged in as a USB hotspot **Safari browses the web**. Rendering runs entirely on the CPU; there is no GPU driver.

> Experimental bring-up, not a usable tablet OS. Many things are missing or stubbed (see below), and Apple's security model is mostly switched off. Do not put anything personal on it.

## What works

| Area | State |
| --- | --- |
| Boot | Pi firmware → our shim (`ios-pi5-shim.img`) → XNU. The Pi's EEPROM and firmware are untouched |
| CPU | All 4 Cortex-A76 cores (PSCI), GICv2 (GIC-400) interrupts and IPIs, 16K pages |
| Memory | 3 GiB for XNU on 4/8/16 GB boards (about 1 GiB on 2 GB boards) |
| Storage | Own SD host (SDHCI) driver, High Speed mode (50 MHz, 4-bit); the iPadOS root is a journaled HFS+ volume on SD partition 3, read/write |
| iPadOS userland | Real iPadOS 16.6.1 root filesystem and shared cache; launchd and the system daemons run |
| Display | The firmware's HDMI framebuffer, presented to iPadOS at its native size (1920×1080 here); CoreAnimation renders in software |
| GUI | backboardd, SpringBoard, lock screen, home screen, dock, App Library, Settings, Messages, Safari |
| Keyboard | USB keyboards (boot protocol) through our own xHCI driver for RP1's USB controllers. **Esc** or **F1** = Home button, **F2** = side (lock) button |
| Mouse | USB mice with the native iPadOS pointer; buttons, wheel and horizontal scroll (report descriptor parsed; `xhciscroll=N` sets the wheel speed) |
| Network | **Internet through an iPhone's Personal Hotspot over USB** (Apple USB Ethernet, like Linux `ipheth`): the link appears as `en1` / "Ethernet" in Settings, iPadOS does DHCP, routing and DNS itself, Safari loads pages. No pairing needed |
| Power button | The Pi's power button is the iPad side button (lock/wake); holding it brings up iPadOS's power-off slider |
| Shut Down / Restart | Settings → General → Shut Down and Restart go to the Pi firmware (PSCI SYSTEM_OFF / SYSTEM_RESET); the root volume is unmounted cleanly first. The power button starts the Pi again. Confirmed on the Pi: Shut Down powers the board off. |
| Device identity | iPad (6th generation), Wi-Fi (`iPad7,5` / `J71bAP`) userland; About shows Model Name "Raspberry Pi 5", Model Number "RPI5" and the board's own serial number |
| Activation | Native "Hactivation" path of mobileactivationd, chosen through device-tree security status. No Apple servers involved |
| Debugging | Every console line (kernel and daemons), with the time since boot, is saved to a log partition on the SD card; crash addresses can be symbolized against the shared cache with `tools/dsc_sym.py` |

## What does not work (yet)

- **No Wi-Fi, Bluetooth, the Pi's own Ethernet port, audio or camera.** The Wi-Fi switch does nothing; Bluetooth is greyed out (its daemon is disabled). Internet works through an iPhone instead (see Network above).
- **No GPU.** Widgets show as white tiles, translucent "material" backgrounds (the dock) come and go, and everything is slower than it should be.
- **Some apps and pages crash:** Photos (the photo library is never created, so its album list is empty) and Settings → General → iPad Storage (the media library has no database, so `[MPMediaLibrary defaultMediaLibrary].databasePath` is nil).
- Our own apps (PiInfo, PiStrap, uicache reload) **appear on the home screen** (confirmed 2026-11-09). They were registered all along; SpringBoard hides apps of type *System* that are not on Apple's default list unless the SpringBoard preference `SBShowNonDefaultSystemApps` is true (what Cydia-era jailbreaks set). The root image now sets it, so a few extra Apple utility apps may show up too; they are left visible for testing.
- **USB hubs** are not supported: plug the keyboard and mouse straight into the Pi.
- **The clock** starts from the time the payload was built unless the network is up (no RTC driver).
- **The first boot of a freshly written root image is slow** (about 10 minutes on the first Apple logo); later boots are much faster.
- The kernel no longer waits 30 s for a real-time clock that this port does not drive (IORTC), so the first Apple logo now passes in a couple of seconds (measured: root mounted at 2.3 s instead of 32 s, SpringBoard at 9.3 s instead of 40 s); the date and time come from `sweettime=` in the payload.
- **Storage is still slowish:** 50 MHz with CPU-copied (PIO) transfers, no DMA or UHS modes yet, so apps take a while to open.
- **Security is off:** permissive code signing (AMFI) and sandbox policies, a stub AppleKeyStore, no data protection.

## How it boots

```
Pi 5 firmware (EEPROM bootloader + start firmware, stock)
  config.txt: kernel=ios-pi5-shim.img, initramfs payload.bin followkernel, pciex4_reset=0
    └─ shim-pi5: entered at EL2, drops to EL1, reads the firmware device tree and
       patches the Apple device tree with this board's serial, RAM and framebuffer
         └─ XNU (MACHINE_CONFIG=SWEET, SWEET_BOARD_PI5): GICv2, PL011 UART, 4 CPUs
              ├─ bsd/dev/sweet_sd.c:   SD card, root = MBR partition type 0xAF (rd=sdroot0)
              ├─ bsd/dev/sweet_xhci.c: RP1 PCIe setup, xHCI, USB HID keyboard/mouse
              └─ HFS+ root → launchd → backboardd → SpringBoard
```

SD card layout (MBR):

| Partition | Type | Contents |
| --- | --- | --- |
| 1 | FAT32 (0x0c) | Pi firmware, `config.txt`, `ios-pi5-shim.img`, `payload.bin` (kernel + device tree + boot-args) |
| 2 | Linux (0x83) | Raspberry Pi OS root, unused |
| 3 | HFS+ (0xAF) | iPadOS 16.6.1 root, 16 GiB |
| 4 | 0xDA | 64 MiB console log ring (`SWEETLOG`) |

## How to do it yourself

You need a Raspberry Pi 5 (developed on the 8 GB model; the shim adapts RAM and display to the others), a microSD card of 32 GB or more with Raspberry Pi OS on it, an HDMI display, a USB keyboard and mouse, and the iPadOS 16.6.1 (20G81) IPSW for iPad7,5 (`iPad_64bit_TouchID_ASTC_16.6.1_20G81_Restore.ipsw`), which you download from Apple yourself. iPadOS is Apple's and is not distributed here.

### 1. Build the kernel

The kernel builds on GitHub Actions (a macOS runner with Xcode): run the **xnu** workflow with `board=pi5`. It applies the patches in [xnu/patches](xnu/patches) to xnu-8796.141.3 and produces `kernel.development.sweet`.

### 2. Build the shim

```bash
shim-pi5/build.sh
```

Needs `gcc-aarch64-linux-gnu`. Produces `shim-pi5/out/ios-pi5-shim.img`.

### 3. Build the iPadOS root image

On Linux (WSL2 works; [tools/wsl-hfsplus.sh](tools/wsl-hfsplus.sh) builds the HFS+ module WSL lacks), with `apfs-fuse`, `hfsprogs` and `rsync`:

```bash
tools/mkiosroot.sh OS.dmg SYSTEMOS.dmg APPOS.dmg ipados-root.img 16G
```

The three images are the IPSW's OS (`096-56598-083.dmg`), `Cryptex1,SystemOS` and `Cryptex1,AppOS` disk images. Then mount the image read-write (`mount -t hfsplus -o loop,force`) and apply the bring-up changes (skip Setup and data migration, remove CommCenter, no panic on daemon crashes, …):

```bash
SWEET_BUILD=20G81 SWEET_VERSION=16.6.1 sudo tools/sweet-root-tweaks.py /mnt/ipados-root
```

### 4. Build the payload

```bash
python3 tools/mkadt_pi5.py pi5.adt
python3 tools/mkpayload.py kernel.development.sweet pi5.adt payload.bin --cmdline \
  "debug=0xc serial=3 cpus=4 keepsyms=1 -noprogress -v rd=sdroot0 sdrw=1 rootrw=1 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweettime=$(date +%s)"
```

### 5. Set up the SD card

- Copy `ios-pi5-shim.img` and `payload.bin` to the boot (FAT) partition and add to the end of `config.txt`:

  ```
  [all]
  kernel=ios-pi5-shim.img
  initramfs payload.bin followkernel
  arm_64bit=1
  os_check=0
  enable_uart=1
  uart_2ndstage=1
  framebuffer_depth=32
  disable_overscan=1
  pciex4_reset=0
  ```

  (Delete these lines to boot Raspberry Pi OS again.)
- Create partition 3 (type `0xAF`, 16 GiB) right after the Raspberry Pi OS partition and write the root image into it raw.
- Optionally create partition 4 (type `0xDA`, 64 MiB) for the console log.

### 6. Read the log

After a boot, copy the first 4 KiB + 4 MiB of partition 4 to a file and decode it:

```bash
python3 tools/sweet-log.py log-area.bin boot.txt
```

## Boot arguments

| Argument | Meaning |
| --- | --- |
| `rd=sdroot0` | Root on the SD card's HFS+ partition |
| `sdrw=1`, `rootrw=1` | Allow writes to that partition; mount the root read-write (needed: the journal is set up on first mount) |
| `sdpart=N` | Use MBR partition N instead of the first type-0xAF one |
| `sdlog=0`, `sdlogsecs=N` | Disable the SD console log; flush interval (default 2 s) |
| `sweetfb_noconsole=1` | Stop drawing kernel text once iPadOS draws its first frame (still logged) |
| `sweettime=SECONDS` | Initial Unix time for the clock (no RTC driver yet) |
| `sdhs=0`, `sdclk=HZ` | Stay at default speed (25 MHz) instead of High Speed; cap the SD clock |
| `xhci=0` | Disable the USB driver |
| `xhcidebug=1` | Log every USB report and interface |
| `sweetfb_hz=N` | How often the frame is copied to the HDMI screen (default 10; the payload now uses 30) |
| `xhciscroll=N` | Scale (negative: invert) the mouse wheel (payload default now 24; 8 was still too slow) |
| `sweethidtest=N` | Press Home every N seconds (input-free testing) |

## Apps

Our own apps live in [apps](apps). They are SwiftUI projects built with [xtool](https://github.com/xtool-org/xtool) (no Mac needed), target iPadOS/iOS 16+, and run on a real iPhone/iPad as well as on the Pi. For the Pi image they are signed ad hoc with [tools/adhoc_sign.py](tools/adhoc_sign.py) and installed as built-in apps in `/Applications`.

| App | What it does |
| --- | --- |
| [PiInfo](apps/PiInfo) | System information: device and board, iPadOS version and build, kernel version, CPU cores, memory, storage, screen, uptime, time zone. On the Pi port it recognizes our kernel and shows **Raspberry Pi 5** as the hardware, with the iPad identity iPadOS runs under (iPad7,5 / J71bAP) labelled as such; on Apple hardware it shows the iPhone/iPad. |
| [PiStrap](apps/PiStrap) | The jailbreak installer, in the style of Dopamine: one **Jailbreak** button plus options for Sileo, Zebra, TrollStore Lite, ElleKit and Cydia (Cydia uses the classic rootful layout, the rest rootless `/var/jb`). **Preview:** the steps (Procursus bootstrap, apt sources, package managers, `uicache`) are shown but not run yet; they need network and the bootstrap packages. |
| [uicache reload](apps/UICacheReload) | Registers apps with iPadOS's app database (LaunchServices), like the jailbreak `uicache` tool, so apps copied into `/Applications` or `/var/jb/Applications` appear on the home screen. Opening it re-registers every non-Apple app and lists the results; a launchd job also runs it once at boot (`--boot`, about 20 s after it starts; no respring needed, SpringBoard picks the apps up live; this build is not yet on the card). It also has a Respring button. No icon. |

Build one with `xtool dev build -c release` in its folder (`xtool dev` installs it on a connected iPhone).

## Repository layout

| Path | Contents |
| --- | --- |
| [xnu/patches](xnu/patches) | Kernel patches: `0001`–`0081` from ios-sweet, `0101-pi5-board-gicv2.patch` for the Pi 5 (GICv2, SMP, SD, USB, HID, …) |
| [shim-pi5](shim-pi5) | Pi 5 boot shim (stage-1 hardware checks without a payload, XNU loader with one) |
| [tools](tools) | Device tree (`mkadt_pi5.py`), payload, root image and root tweaks, log decoder (`sweet-log.py`), ad-hoc signer with entitlements (`adhoc_sign.py`), shared-cache symbolizer (`dsc_sym.py`), changed-block writer for the SD root (`blockdelta.py` + `sd-delta-write.ps1`) |
| [apps](apps) | Our iPadOS apps: PiInfo, PiStrap, uicache reload (see Apps) |
| [PI5-PORT.md](PI5-PORT.md) | Detailed Pi 5 port notes: hardware facts, boot logs, what was tried |
| [NOTES.md](NOTES.md) | The phone port's development notes |
| `shim`, `site`, `docs` | The phone port's boot shim, website and docs |

## Key hardware facts

| | |
| --- | --- |
| SoC | Broadcom BCM2712, 4× Cortex-A76 (r4p1), MPIDR 0x81000n00 |
| Interrupts | GIC-400 (GICv2): GICD 0x107fff9000, GICC 0x107fffa000 |
| Timer | 54 MHz, virtual timer PPI 27 |
| UART | PL011 at 0x107d001000 (3-pin debug header) |
| SD | brcm,bcm2712-sdhci at 0x107cfff000 (+ cfg 0x107cfff400), 200 MHz base clock |
| USB | RP1 (PCIe x4) DWC3 xHCI ×2 at RP1 BAR1 + 0x200000/0x300000; the firmware maps RP1 at CPU 0x1c00000000 → PCI 0x80000000 (root port bus numbers and inbound DMA window are set up by the kernel) |
| iPhone tethering | Configuration 4, interface 2 alt 1 (class ff/fd/01), bulk in/out; frames carry 2 bytes of padding on receive |
| Power button | GPIO 20 of the brcmstb GPIO block at 0x107d508500, active low |

## Credits

Built on [ios-sweet](https://github.com/dipa-studio/ios-sweet) by dipa-studio and everything it credits: Apple's open-source releases (xnu, IOHIDFamily, libpthread, hfs), [BearSSL](https://bearssl.org), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build) and [ipsw](https://github.com/blacktop/ipsw). Pi 5 hardware details come from the Raspberry Pi firmware's device trees and the published Linux drivers; [apfs-fuse](https://github.com/sgan81/apfs-fuse) reads the IPSW's images. iPadOS itself is Apple's and is not distributed here.

MIT License, see [LICENSE](LICENSE).
