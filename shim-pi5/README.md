# Pi 5 shim, stage 1

A tiny bare-metal program the Pi 5 firmware boots in place of Linux. It
checks everything the XNU port depends on and prints the results on the
debug UART and on HDMI. Nothing is written to the SD card or the EEPROM.

What it checks:

- it was entered at EL2 and dropped to EL1 (where XNU runs)
- the CPU is a Cortex-A76 with the 16K page granule, no PAC, no GIC system registers
- RAM, UART, GIC-400, timer and framebuffer addresses from the device tree
- a GICv2 interrupt from the virtual timer reaches EL1 (~30 in 300 ms)
- PSCI `CPU_ON` starts cpu1-3 and each one reports in

## Build (WSL Ubuntu)

```bash
sudo apt install gcc-aarch64-linux-gnu
```

```bash
./build.sh
```

Output: `out/ios-pi5-shim.img`.

## Run

1. Flash Raspberry Pi OS Lite to an SD card with Raspberry Pi Imager (it is
   only used for its boot partition: firmware config and the Pi 5 device tree).
2. On the `bootfs` partition: copy `out/ios-pi5-shim.img` there, back up
   `config.txt`, and append the lines from `shim-pi5/config.txt` at the end.
3. Connect a USB-UART probe to the 3-pin debug header between the HDMI ports
   (115200 8N1), and an HDMI monitor to HDMI0 (the one next to USB-C power).
4. Power on. Firmware logs appear on the UART, then the shim's report.

To go back to Linux, remove the appended lines from `config.txt`.

## What a good run looks like

Every line `[ OK ]` and `STAGE 1 PASSED` at the end, on both UART and HDMI.
Send the full UART log back: the GIC, timer and memory values in it feed the
stage 2 board config and ADT.

If HDMI stays black but the UART works, comment out `dtoverlay=vc4-kms-v3d`
in `config.txt`. If the UART is silent too, the screen may still show a red
or blue panic page with the exception registers.
