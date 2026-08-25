# Hardware Requirements

## Hardware

- **Board:** BeagleBone Black (AM335x, Debian Trixie, kernel 6.6.58-ti-rt-arm32)
- **GPS options tested:** u-blox NEO-6M, NEO-M8T (connected via UART4 P9_11 RX, P9_13 TX)
  - Note: NEO-M9N does not have a UART interface and is not suitable here
- **DS3231 on I2C2**, doing three jobs:
  - RTC for time holdover across power loss
  - its 32.768 kHz output into the PRU as a ±2 ppm temperature-compensated frequency reference; the daemon learns the residual tempco against GPS and holds the timebase through GPS outages
  - die temperature readout for the tempco model
- **Ethernet:** the on-chip cpsw MAC. Its CPTS engine cannot timestamp NTP, which is why PRU1 stamps every frame instead

## Pinout

| Pin | Offset | Mode | Signal |
|------|--------|------|--------|
| **P8_15** | 0x3c | 5, `ecap0_capin` | GPS PPS into eCAP CAP1 |
| **P9_41A** | 0x1b4 | 5, `pru0_r31_16` | DS3231 32.768 kHz |
| **P9_41B** | 0x1a8 | 7, gpio in | parked companion pad |
| P9_11/13 | | UART4 RX/TX | GPS serial, 115200 |
| P9_19/20 | | I2C2 | DS3231 RTC/temp |

P8_15 and P9_41A carry input-enable and pull-up. The 32 kHz output is open drain:
the internal ~27k pull-up carries it, and PRU0 counts the falling edges the RTC
drives hard, so the slow pulled rise never matters. P9_41B is the companion pad of
the same header pin, muxed to a plain input so it cannot contend.

## Prerequisites

### Kernel & Toolchain

```bash
# Confirm RT kernel
uname -r
# 6.6.58-ti-rt-arm32-r12

# PRU compiler (TI Code Generation Tools): should already be installed
ls /usr/share/ti/cgt-pru/bin/clpru

# Device tree compiler
apt install device-tree-compiler

# PRU Software Support Package (PSSP): clone from TI's git
# This provides headers (pru_cfg.h, pru_intc.h, pru_rpmsg.h, etc.)
# and the rpmsg_lib.lib needed to build the firmware.
git clone https://git.ti.com/git/pru-software-support-package/pru-software-support-package.git \
  /opt/source/pssp
```

The build expects these paths:
- **`PRU_CGT=/usr/share/ti/cgt-pru`**: TI PRU compiler toolchain (pre-installed on BeagleBone images)
- **`PSSP=/opt/source/pssp`**: PRU Software Support Package (cloned above)

Kernel headers for the running kernel are needed to build the two modules in
[`kernel/`](../kernel/).

[Next: Initial Setup & Overlays](setup.md) | [Previous: Architecture & Overview](architecture.md)
