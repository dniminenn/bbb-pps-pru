# PRU PPS: a nanosecond-precision GPS Stratum 1 clock on the BeagleBone Black

**Writeup:** [BeagleBone PRU PPS timestamping for precise GPS time](https://dnim.dev/blog/bbb-pru-pps-timestamping)

Turn a BeagleBone Black and a GPS module with a PPS output into a **Stratum 1 NTP
server** whose time is disciplined to **nanoseconds** and whose served packets carry
**hardware timestamps**, using both PRU cores of the AM335x:

- **PRU0** owns the reference. The GPS pulse-per-second edge is latched **in silicon**
  by the ICSS eCAP capture unit (CAP1, rising) against a free-running 200 MHz counter,
  so there is no polling loop and no interrupt anywhere in the capture path. The same
  core counts the 32.768 kHz output of a DS3231 TCXO as an independent,
  temperature-compensated frequency reference.
- **PRU1** timestamps **every Ethernet frame on the wire** into a shared ring, giving
  the cpsw MAC the hardware RX/TX packet timestamps the silicon never had. A small
  kernel module pair (`cpsw_pruts` + `ptp_pruss`) feeds those stamps to the kernel
  timestamping API and exposes the eCAP timebase as a PTP hardware clock
  (`/dev/ptpX`, phase at TAI, frequency trimmed to GPS).

chrony reads the PPS directly from the PHC as an **extts event** (no userspace
middleman in the reference path) and serves NTP with hardware transmit and receive
timestamps, interleaved mode included.

<img src="diagrams/pipeline.svg" width="340" alt="PRU PPS data path: GPS PPS captured by the PRU, fed to chrony">

## Performance Results

- **Edge capture:** latched by the eCAP capture unit in hardware; the capture itself
  has no software jitter term. The receiver's per-pulse quantization error
  (UBX-TIM-TP `qErr`, ±10 ns class) is matched to its pulse by GPS week/tow and
  subtracted.
- **Timebase transfer:** the daemon's least-squares fit of eCAP ticks against the
  unsteered system timeline runs at **11 to 18 ns RMS** residuals.
- **System clock:** `chronyc tracking` RMS offset sits at **30 to 70 ns** against the
  PPS once the servo converges.
- **Served time (the number that matters):** measured from two independent
  GPS-disciplined observers with hardware-timestamped NTP (interleaved, delay-gated),
  this server agrees to **~100 ns** median, hour-scale p95 under 400 ns.
- **Load immunity:** through a 90 s full-CPU torture test the extts reference path
  held 88 ns RMS with no sample past 500 ns.

Detailed tracking data and logs: [Measurements & Validation](docs/measurements.md).

### PTP cross-check against the LAN's grandmaster

A measure-only `ptp4l` (client, free-running) rides the PRU1 packet stamps and
compares the elected PTP grandmaster (an Intel i210 + u-blox receiver whose PPS is
SDP-captured and disciplined by `ts2phc`) against this box's GPS, anchored to the
same eCAP PPS capture. The instrument reads the grandmaster within **~0.5 µs**, with
the residual dominated by fixed frame-length constants, not the clocks. Method and
stamp-convention details: [measurement/](measurement/). The earlier CPTS-based
experiment is preserved in [CROSS-VALIDATION.md](CROSS-VALIDATION.md).

## Documentation

1. **[Architecture & Overview](docs/architecture.md)**: both PRUs, the kernel modules, and the data paths
2. **[Hardware Requirements](docs/hardware.md)**: supported hardware, GPS modules, DS3231, pinout
3. **[Initial Setup & Overlays](docs/setup.md)**: `uEnv.txt` configuration and device tree overlays
4. **[PRU Firmware](docs/firmware.md)**: compiling and installing both PRU firmwares
5. **[Userspace Daemon & Chrony](docs/daemon.md)**: the calibration daemon, the extts refclock, chrony configuration
6. **[Verification & Troubleshooting](docs/verification.md)**: checking system behavior, remoteproc state, and common issues

### Deep Dives

7. **[Timestamp Model](docs/timestamp-model.md)**: what exactly is captured, which edge, what defines t=0
8. **[Clock Domains & Synchronization](docs/clock-domains.md)**: eCAP to wall correlation, the two fits, ordering guarantees
9. **[Measurements & Validation](docs/measurements.md)**: expected performance, real-world tracking/statistics, diagnostics

## Source Code

- [Firmware (`firmware/`)](firmware/): PRU0 eCAP/TCXO capture, PRU1 packet-timestamp ring, resource tables
- [Kernel (`kernel/`)](kernel/): `ptp_pruss` (eCAP timebase as a PHC, PPS as extts channel 0), `cpsw_pruts` (PRU ring stamps into the kernel timestamping path)
- [Daemon (`daemon/`)](daemon/): calibration daemon (SHM witness feed, DS3231 tempco learning, PHC trim and anchor), systemd services
- [Chrony (`chrony/`)](chrony/): the extts refclock configuration and two small patches
- [Overlays (`overlays/`)](overlays/): device tree overlays for pinmux and rpmsg
- [Measurement (`measurement/`)](measurement/): measure-only ptp4l and the grandmaster-agreement exporter
- [Tools (`tools/`)](tools/): log parsing and statistics helpers
