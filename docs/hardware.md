# Hardware Requirements

## Hardware

- **Board:** BeagleBone Black (AM335x, Debian Trixie, kernel 6.6.58-ti-rt-arm32)
- **GPS options tested:** u-blox NEO-6M, NEO-M8T — connected via UART4 (P9_11 RX, P9_13 TX)
  - Note: NEO-M9N does **not** have a UART interface and is not suitable here
- **PPS pin:** P8_16 → GPIO1_14 → PRU0 R31 bit 14
- **RTC:** DS3231 on I2C2 (holdover)

## Prerequisites

### Kernel & Toolchain

```bash
# Confirm RT kernel
uname -r
# 6.6.58-ti-rt-arm32-r12

# PRU compiler — TI clpru must be in PATH
clpru --version

# Device tree compiler
apt install device-tree-compiler

# PRU support headers required:
#   pru_cfg.h, pru_intc.h, pru_rpmsg.h, pru_virtqueue.h,
#   rsc_types.h, pru_virtio_ids.h
# These come from the TI PRU Software Support Package (pru-software-support-package)
# or /usr/lib/ti/pru-software-support-package on BeagleBone Debian images
```

[Next: Initial Setup & Overlays](setup.md) | [Previous: Architecture & Overview](architecture.md)
