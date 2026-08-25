# Initial Setup & Overlays

## uEnv.txt Configuration

The following overlays must be active in your `/boot/uEnv.txt`:

```
enable_uboot_overlays=1

# UART4 for GPS NMEA
uboot_overlay_addr4=/lib/firmware/BB-UART4-00A0.dtbo

# PRU remoteproc with vring (rpmsg) support
uboot_overlay_addr5=/lib/firmware/PRU-RPROC-VRING-00A0.dtbo

# DS3231 RTC on I2C2
uboot_overlay_addr6=/lib/firmware/BB-I2C2-DS3231-00A0.dtbo

# PRU PPS pinmux (P8_15 eCAP capture + P9_41A DS3231 32kHz)
uboot_overlay_addr7=/lib/firmware/PRU-PPS-ECAP-00A0.dtbo

# PRU remoteproc driver (4.19-ti style, works on 6.6-ti)
uboot_overlay_pru=AM335X-PRU-RPROC-4-19-TI-00A0.dtbo

# Disable unused overlays to reduce noise
disable_uboot_overlay_video=1
disable_uboot_overlay_audio=1

# RT/timing-optimized cmdline
cmdline=fsck.repair=yes earlycon coherent_pool=1M net.ifnames=0 lpj=1990656 \
  rng_core.default_quality=100 intel_idle.max_cstate=0 processor.max_cstate=1 idle=poll
```

## Device Tree Overlays

### [PRU-PPS-ECAP-00A0.dts](../overlays/PRU-PPS-ECAP-00A0.dts)

Configures P8_15 (offset 0x3c) as `pr1_ecap0_ecap_capin` (mode 5, input, pull-up) so the eCAP capture unit sees the PPS, P9_41A (offset 0x1b4) as PRU0 R31 bit 16 for the DS3231 32.768 kHz output, and parks the P9_41B companion pad as a plain input so it cannot contend. See the [pinout table](hardware.md#pinout).

Compile and install:

```bash
dtc -O dtb -o /lib/firmware/PRU-PPS-ECAP-00A0.dtbo -b 0 -@ overlays/PRU-PPS-ECAP-00A0.dts
```
### [PRU-RPROC-VRING-00A0.dts](../overlays/PRU-RPROC-VRING-00A0.dts)

This overlay wires the PRU0/PRU1 interrupt lines for rpmsg/vring communication:

- PRU0: `vring` (sysevt 16, ch 2, host 2), `kick` (sysevt 17, ch 0, host 0)
- PRU1: `vring` (sysevt 18, ch 3, host 3), `kick` (sysevt 19, ch 1, host 1)

Compile and install:

```bash
dtc -O dtb -o /lib/firmware/PRU-RPROC-VRING-00A0.dtbo -b 0 -@ overlays/PRU-RPROC-VRING-00A0.dts
```

[Next: PRU Firmware](firmware.md) | [Previous: Hardware Requirements](hardware.md)
