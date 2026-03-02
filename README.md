# PRU PPS Guide: BeagleBone Black PRU Hardware Timestamping → Chrony SHM

A complete guide to implementing a PRU-based hardware-timestamped PPS source on the AM335x BeagleBone Black, feeding nanosecond-precision timing into Chrony via shared memory.

```
  PRU-ICSS (edge capture)              Linux (servo / policy)
 ┌──────────────────────────┐    ┌───────────────────────────────┐
 │ GPS PPS → P8_16 → R31    │    │ pru_pps_shm (SCHED_FIFO:50)   │
 │ PRU0 polls rising edge   ├───►│ IEP↔wall calibration          │
 │ IEP latch (200 MHz)      │    │ NTP SHM unit 2                │
 │ DRAM0: { seq, iep_lo }   │    │ Chrony refclock → adjtimex    │
 └──────────────────────────┘    └───────────────────────────────┘
```

## Documentation

1. **[Architecture & Overview](docs/architecture.md)**: What is a PRU, why use it for PPS, and the system architecture
2. **[Hardware Requirements](docs/hardware.md)**: Supported hardware, GPS modules, and prerequisites
3. **[Initial Setup & Overlays](docs/setup.md)**: `uEnv.txt` configuration and device tree overlays
4. **[PRU Firmware](docs/firmware.md)**: Compiling and installing the PRU microcontroller firmware
5. **[Userspace Daemon & Chrony](docs/daemon.md)**: Building the shared memory daemon and configuring Chrony
6. **[Verification & Troubleshooting](docs/verification.md)**: Checking system behavior, remoteproc state, and common issues

### Deep Dives

7. **[Timestamp Model](docs/timestamp-model.md)**: What exactly is captured, which edge, what defines t=0
8. **[Clock Domains & Synchronization](docs/clock-domains.md)**: IEP to wall correlation, memory mechanism, ordering guarantees
9. **[Measurements & Validation](docs/measurements.md)**: Expected performance, real-world tracking/statistics, and diagnostic guidance

## Source Code

- [Firmware (`firmware/`)](firmware/): PRU0 C source, resource table, INTC map, linker command
- [Daemon (`daemon/`)](daemon/): Userspace SHM bridge and systemd service
- [Overlays (`overlays/`)](overlays/): Device tree overlays for pinmux and rpmsg
- [Tools (`tools/`)](tools/): Log parsing and statistics helpers
