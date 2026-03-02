# PRU PPS Guide: BeagleBone Black PRU Hardware Timestamping → Chrony SHM

A complete guide to implementing a PRU-based hardware-timestamped PPS source on the AM335x BeagleBone Black, feeding nanosecond-precision timing into Chrony via shared memory.

## Documentation Wiki

Welcome to the PRU PPS documentation. For a step-by-step guide on how this system works and how to set it up, please follow the documentation below:

1. **[Architecture & Overview](docs/architecture.md)** — What is a PRU, why use it for PPS, and the system architecture.
2. **[Hardware Requirements](docs/hardware.md)** — Supported hardware, GPS modules, and prerequisites.
3. **[Initial Setup & Overlays](docs/setup.md)** — `uEnv.txt` configuration and device tree overlays.
4. **[PRU Firmware](docs/firmware.md)** — Compiling and installing the PRU microcontroller firmware.
5. **[Userspace Daemon & Chrony](docs/daemon.md)** — Building the shared memory daemon and configuring Chrony.
6. **[Verification & Troubleshooting](docs/verification.md)** — Checking system behavior, remoteproc state, and common issues.

## Source Code

- [Firmware Source (`firmware/`)](firmware/)
- [Userspace Daemon (`daemon/`)](daemon/)
- [Device Tree Overlays (`overlays/`)](overlays/)
