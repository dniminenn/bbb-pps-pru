# PRU PPS Guide: BeagleBone Black PRU Hardware Timestamping → Chrony SHM

A complete guide to implementing a PRU-based hardware-timestamped PPS source on the AM335x BeagleBone Black, feeding nanosecond-precision timing into Chrony via shared memory.

---

## What is a PRU?

The Programmable Real-Time Unit (PRU) subsystem on the AM335x contains two 200-MHz microcontrollers that run completely independently of the main ARM CPU and the Linux kernel. They are configured, loaded with firmware, and booted from userspace via the Linux **remoteproc** framework. 

Because PRUs do not run an operating system or handle interrupts in the traditional sense, their execution is completely deterministic and strictly real-time. They can poll pins, manipulate hardware registers, and interact with peripherals at cycle-accurate precision (5 ns per instruction). This allows them to bypass the scheduling jitter and latency of traditional Linux GPIO interrupt handlers, making them perfect for ultra-precise hardware timestamping.

---

## Overview

### Why not GPIO PPS?

The standard Linux GPIO PPS driver (`pps-gpio`) timestamps the PPS edge in a GPIO interrupt handler. While better than serial PPS, it still goes through the interrupt subsystem — yielding roughly **20 µs dispersion** with **10 µs+ outliers** under normal system load.

This approach uses **PRU0** to timestamp the PPS edge directly using the IEP (Industrial Ethernet Peripheral) free-running counter at **200 MHz (5 ns/tick)**. The PRU sees the pin transition with zero interrupt latency — it polls `R31` in a tight loop. A userspace daemon reads the timestamp from PRU DRAM via `/dev/mem` and writes it into Chrony's NTP SHM refclock.

**Achieved accuracy:** offsets consistently in the 100–800 ns range, ~1.3 µs calibration spread, completely bypassing the GPIO interrupt path.

### Architecture

```
GPS module PPS pin
      │
      ▼
P8_16 (GPIO1_14) ──── PRU0 R31 bit 14
                            │
                    IEP counter latch (200 MHz, 5 ns/tick)
                            │
                    PRU DRAM0 @ 0x4A300000
                    struct { seq, iep_lo }
                            │
                    pru_pps_shm  (userspace, SCHED_FIFO:50)
                    /dev/mem mmap + IEP wall calibration
                            │
                    NTP SHM unit 2
                            │
                    Chrony refclock SHM 2 (PPS, 1e-9 precision)
```

---

## Hardware

- **Board:** BeagleBone Black (AM335x, Debian Trixie, kernel 6.6.58-ti-rt-arm32)
- **GPS options tested:** u-blox NEO-6M, NEO-M8T — connected via UART4 (P9_11 RX, P9_13 TX)
  - Note: NEO-M9N does **not** have a UART interface and is not suitable here
- **PPS pin:** P8_16 → GPIO1_14 → PRU0 R31 bit 14
- **RTC:** DS3231 on I2C2 (holdover)

---

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

---

## uEnv.txt Configuration

The following overlays must be active:

```
enable_uboot_overlays=1

# UART4 for GPS NMEA
uboot_overlay_addr4=/lib/firmware/BB-UART4-00A0.dtbo

# PRU remoteproc with vring (rpmsg) support
uboot_overlay_addr5=/lib/firmware/PRU-RPROC-VRING-00A0.dtbo

# DS3231 RTC on I2C2
uboot_overlay_addr6=/lib/firmware/BB-I2C2-DS3231-00A0.dtbo

# PRU PPS pinmux — configures P8_16 as PRU input
uboot_overlay_addr7=/lib/firmware/PRU-PPS-PINMUX-00A0.dtbo

# PRU remoteproc driver (4.19-ti style, works on 6.6-ti)
uboot_overlay_pru=AM335X-PRU-RPROC-4-19-TI-00A0.dtbo

# Disable unused overlays to reduce noise
disable_uboot_overlay_video=1
disable_uboot_overlay_audio=1

# RT/timing-optimized cmdline
cmdline=fsck.repair=yes earlycon coherent_pool=1M net.ifnames=0 lpj=1990656 \
  rng_core.default_quality=100 intel_idle.max_cstate=0 processor.max_cstate=1 idle=poll
```

---

## Device Tree Overlays

### PRU-PPS-PINMUX-00A0.dts

Configures P8_16 (offset 0x38) as a PRU input with pull-down disabled, input enabled, mode 6 (pr1_pru0_pru_r31_14).

```dts
/* See PRU-PPS-PINMUX-00A0.dts */
```

Compile and install:

```bash
dtc -O dtb -o /lib/firmware/PRU-PPS-PINMUX-00A0.dtbo -b 0 -@ PRU-PPS-PINMUX-00A0.dts
```

### PRU-RPROC-VRING-00A0.dtbo

This overlay wires the PRU0/PRU1 interrupt lines for rpmsg/vring communication:

- PRU0: `vring` (sysevt 16, ch 2, host 2), `kick` (sysevt 17, ch 0, host 0)
- PRU1: `vring` (sysevt 18, ch 3, host 3), `kick` (sysevt 19, ch 1, host 1)

```dts
/* See PRU-RPROC-VRING-00A0.dts */
```

Compile and install:

```bash
dtc -O dtb -o /lib/firmware/PRU-RPROC-VRING-00A0.dtbo -b 0 -@ PRU-RPROC-VRING-00A0.dts
```

---

## PRU Firmware

### Project Layout

```
/opt/source/pru-pps/
├── main.c                    # PRU0 firmware
├── resource_table.h          # rpmsg vdev resource table
├── resource_table_min.c      # minimal resource table (no rpmsg, unused)
├── intc_map_0.h              # INTC interrupt map
├── AM335x_PRU_intc_rscTbl.cmd  # linker command file
└── gen/
    ├── pru-pps.out           # compiled firmware binary
    └── pru-pps.map           # linker map
```

### main.c — PRU0 Firmware

The PRU firmware:
1. Initializes rpmsg (required for remoteproc to consider the PRU "ready")
2. Starts the IEP counter in free-running mode at 200 MHz
3. Polls `R31` bit 14 for a rising edge on P8_16
4. On each rising edge, latches `IEP_COUNT_LO` and increments a sequence number
5. Stores `{seq, iep_lo}` in PRU DRAM0 at offset 0x0 (physical 0x4A300000)

```c
/* See main.c */
```

### resource_table.h

Declares one rpmsg vdev with two vrings (VQ size 16 each). Required for the remoteproc driver to set up the rpmsg channel and mark the PRU as running.

Key defines:
- `CHAN_NAME "rpmsg-pru"` — matches what the kernel rpmsg_pru driver expects
- `CHAN_PORT 30` — creates `/dev/rpmsg_pru30`

### intc_map_0.h

Maps sysevt 17 → channel 0 → host interrupt 0. This is the kick interrupt from the ARM host to PRU0.

```c
/* See intc_map_0.h */
```

### AM335x_PRU_intc_rscTbl.cmd — Linker Command File

Key section placement:

```
.pps_dram  > 0x0, PAGE 1    /* PRU DRAM0 offset 0 = physical 0x4A300000 */
.resource_table > PRU_DMEM_0_1, PAGE 1
.text      > PRU_IMEM, PAGE 0
```

The `.pps_dram` section at offset 0 of PAGE 1 (PRU DRAM0) means the `pps_shared` struct is always at physical address `0x4A300000` — hardcoded in the userspace daemon.

### Building the Firmware

```bash
cd /opt/source/pru-pps

# Compile
clpru -v3 -O2 --include_path=/usr/lib/ti/pru-software-support-package/include \
      --include_path=/usr/lib/ti/pru-software-support-package/include/am335x \
      -DCORE0 main.c -o gen/main.obj

# Link
clpru -v3 -z AM335x_PRU_intc_rscTbl.cmd gen/main.obj \
      --library=/usr/lib/ti/pru-software-support-package/lib/rpmsg_lib.lib \
      -o gen/pru-pps.out \
      --map_file=gen/pru-pps.map

# Install firmware
cp gen/pru-pps.out /lib/firmware/am335x-pru0-fw
```

The remoteproc driver loads `/lib/firmware/am335x-pru0-fw` by default for PRU0.

---

## Userspace Daemon: pru_pps_shm

### How It Works

1. **`/dev/mem` mmap** — maps PRU DRAM0 (`0x4A300000`) and IEP registers (`0x4A32E000`)
2. **Polls `pru->seq`** — detects new PPS edges at ~50 µs poll interval
3. **IEP–wall calibration** — takes 10 back-to-back `clock_gettime(CLOCK_REALTIME)` + IEP read samples, keeps the tightest bracket to get the most accurate mapping between IEP ticks and wall time
4. **ns/tick estimation** — uses consecutive IEP deltas (should be ~200,011,500 ticks per second at ~5 ns/tick) with a 0.9/0.1 IIR filter
5. **Projects PPS wall time** — `pps_wall_ns = cal_wall + (pps_iep - cal_iep) * ns_per_tick`
6. **Writes NTP SHM** — using mode-1 count handshake (odd while writing, even when done)

### SHM Struct Layout

The NTP SHM struct uses 64-bit `time_t` (Y2038-safe on this Debian Trixie system even on 32-bit ARM):

| Offset | Field | Notes |
|--------|-------|-------|
| 0 | `mode` (int32) | Set to 1 |
| 4 | `count` (int32) | Handshake: odd=writing, even=done |
| 8 | `clockTimeStampSec` (int64) | Rounded UTC second the pulse represents |
| 16 | `clockTimeStampUSec` (int32) | Always 0 — PPS is on-second |
| 24 | `receiveTimeStampSec` (int64) | Wall time when edge was detected |
| 32 | `receiveTimeStampUSec` (int32) | Sub-second µs |
| 36 | `leap` (int32) | 0 |
| 40 | `precision` (int32) | -29 (~2 ns) |
| 56 | `clockTimeStampNSec` (uint32) | Always 0 |
| 60 | `receiveTimeStampNSec` (uint32) | Sub-second ns |

Chrony uses `clockTS - receiveTS` as its raw offset sample, so `receiveTimeStamp` must be the best available wall-clock estimate of the actual edge time — not rounded.

### Full Source: pru_pps_shm.c

```c
/* See pru_pps_shm.c */
```

### Compile and Install

```bash
gcc -O2 -o /usr/local/bin/pru_pps_shm /opt/source/pru-pps/pru_pps_shm.c
```

No special flags needed — links against standard libc only.

---

## Systemd Service

`/etc/systemd/system/pru-pps-shm.service`:

```ini
# See pru-pps-shm.service
```

```bash
systemctl daemon-reload
systemctl enable --now pru-pps-shm
```

**Note:** `remoteproc1` is PRU0 (`4a334000.pru`). `remoteproc2` is PRU1. The `ExecStartPre` sleep gives the rpmsg channel time to initialize before the daemon starts polling.

---

## Chrony Configuration

Relevant section of `/etc/chrony/chrony.conf`:

```
sched_priority 80

# GPS NMEA via gpsd/SHM unit 0 — used for coarse time and lock reference
refclock SHM 0 refid GPS precision 1e-1 delay 0.4 poll 2 trust noselect

# PRU PPS via SHM unit 2 — primary disciplining source
refclock SHM 2 refid PPS precision 1e-9 poll 1 prefer trust lock GPS pps

makestep 0.5 -1
maxupdateskew 100.0
rtcsync
rtcdevice /dev/rtc0
```

`lock GPS` tells Chrony to only use the PPS refclock when the GPS SHM refclock (unit 0) is also valid — essential for correct UTC second assignment. `pps` enables the PPS refclock mode.

---

## Verifying the Setup

### Check remoteproc state

```bash
cat /sys/class/remoteproc/remoteproc1/state   # should be: running
cat /sys/class/remoteproc/remoteproc1/firmware # should be: am335x-pru0-fw
```

### Check rpmsg channel created

```bash
dmesg | grep -i pru
# remoteproc remoteproc1: Booting fw image am335x-pru0-fw, size 73268
# virtio_rpmsg_bus virtio0: creating channel rpmsg-pru addr 0x1e
# rpmsg_pru virtio0.rpmsg-pru.-1.30: new rpmsg_pru device: /dev/rpmsg_pru30
```

### Check daemon output

```bash
journalctl -u pru-pps-shm -f
```

Expected output:
```
pru_pps_shm: polling PRU DRAM -> SHM unit 2
seq=1234 delta=200011550 offset=+352 ns gap=18354 (91.8 us) spread=1291 ns ns/tick=4.999711 [good=3411]
```

- `delta` — IEP ticks between consecutive PPS pulses (~200,011,500 for 200 MHz IEP with slight trim)
- `offset` — sub-second residual of the detected edge (should be <1 µs steady-state)
- `gap` — IEP ticks between PPS edge and calibration sample (how stale our cal is)
- `spread` — nanoseconds between the two bracketing `clock_gettime` calls (quality of wall cal)

### Check Chrony

```bash
chronyc sources -v
# PPS source should show * (selected) with offset in tens of nanoseconds
```

---

## Remoteproc Map

| remoteproc | PRU | Physical | Firmware |
|------------|-----|----------|----------|
| remoteproc0 | PM firmware | — | am335x-pm-firmware.elf |
| remoteproc1 | PRU0 | 0x4a334000 | am335x-pru0-fw |
| remoteproc2 | PRU1 | 0x4a338000 | am335x-pru1-fw |

PRU DRAM0 base: `0x4A300000` (mapped read-only by pru_pps_shm)  
IEP base: `0x4A32E000` (mapped read-only for live counter reads during calibration)

---

## Troubleshooting

**PRU stays offline / firmware not loading**
- Confirm `am335x-pru0-fw` exists in `/lib/firmware/` and is the correct `.out` binary
- Check `PRU-RPROC-VRING-00A0.dtbo` and `AM335X-PRU-RPROC-4-19-TI-00A0.dtbo` are both active
- Try manually: `echo start > /sys/class/remoteproc/remoteproc1/state`

**No rpmsg channel / `/dev/rpmsg_pru30` missing**
- The firmware must initialize rpmsg and call `pru_rpmsg_channel()` before the channel appears
- `PRU-RPROC-VRING-00A0.dtbo` must be loaded — it wires the interrupt lines

**seq never increments**
- Confirm P8_16 pinmux is correct: `0x38 0x36` (mode 6, input enabled, pull disabled)
- Verify PPS signal present on P8_16 with a multimeter or scope
- Check `PRU-PPS-PINMUX-00A0.dtbo` is loaded at boot

**Large offsets or bad samples**
- `bad` counter increments when `iep_delta` is outside 150M–250M ticks (sanity check for 1 Hz)
- Large `gap` values (>500 µs) mean the daemon didn't get scheduled quickly after the edge — check `CPUSchedulingPolicy=fifo` is effective (`sched_setscheduler` succeeds)
- Thermal drift: IEP frequency shifts with temperature; the IIR filter adapts but needs a few minutes to settle
