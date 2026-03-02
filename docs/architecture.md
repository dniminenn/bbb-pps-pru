# Architecture & Overview

## What is a PRU?

The Programmable Real-Time Unit (PRU) subsystem on the AM335x contains two 200-MHz microcontrollers that run completely independently of the main ARM CPU and the Linux kernel. They are configured, loaded with firmware, and booted from userspace via the Linux **remoteproc** framework. 

Because PRUs do not run an operating system or handle interrupts in the traditional sense, their execution is completely deterministic and strictly real-time. They can poll pins, manipulate hardware registers, and interact with peripherals at cycle-accurate precision (5 ns per instruction). This allows them to bypass the scheduling jitter and latency of traditional Linux GPIO interrupt handlers, making them perfect for ultra-precise hardware timestamping.

## Why not GPIO PPS?

The standard Linux GPIO PPS driver (`pps-gpio`) timestamps the PPS edge in a GPIO interrupt handler. While better than serial PPS, it still goes through the interrupt subsystem — yielding roughly **20 µs dispersion** with **10 µs+ outliers** under normal system load.

This approach uses **PRU0** to timestamp the PPS edge directly using the IEP (Industrial Ethernet Peripheral) free-running counter at **200 MHz (5 ns/tick)**. The PRU sees the pin transition with zero interrupt latency — it polls `R31` in a tight loop. A userspace daemon reads the timestamp from PRU DRAM via `/dev/mem` and writes it into Chrony's NTP SHM refclock.

**Achieved accuracy:** offsets consistently in the 100–800 ns range, ~1.3 µs calibration spread, completely bypassing the GPIO interrupt path.

## Architecture

```
 ┌──────────────────────────────────────────────────────────────┐
 │  PRU-ICSS  (edge capture / phase detector)                  │
 │                                                              │
 │   GPS PPS ──► P8_16 (pr1_pru0_pru_r31_14)                  │
 │                     │                                        │
 │              PRU0 polls R31 in tight loop (rising edge)      │
 │                     │                                        │
 │              IEP counter latch (200 MHz, 5 ns/tick)          │
 │                     │                                        │
 │              PRU DRAM0 @ 0x4A300000                          │
 │              struct { seq, iep_lo }    ◄─── raw IEP ticks    │
 └─────────────────────┬────────────────────────────────────────┘
                       │  /dev/mem mmap (read-only)
 ┌─────────────────────▼────────────────────────────────────────┐
 │  Linux  (clock-domain crossing + servo / policy)             │
 │                                                              │
 │   pru_pps_shm  (SCHED_FIFO:50)                              │
 │      ├── IEP ↔ wall calibration (10-sample tight bracket)   │
 │      ├── ns/tick IIR filter (tracks thermal drift)           │
 │      └── projects PPS edge to CLOCK_REALTIME                 │
 │                     │                                        │
 │              NTP SHM unit 2 (mode-1 handshake)               │
 │                     │                                        │
 │              Chrony refclock SHM 2                            │
 │              (PPS, precision 1e-9, lock GPS)                  │
 │                     │                                        │
 │              System clock disciplined via adjtimex            │
 └──────────────────────────────────────────────────────────────┘
```

### Component roles

- **PRU** = deterministic edge capture and phase detector. It captures *when* relative to its own clock the PPS edge happened. It produces raw IEP ticks, not wall time.
- **Daemon** = clock-domain bridge. Translates IEP ticks → `CLOCK_REALTIME` nanoseconds via the calibration loop.
- **Chrony** = servo and policy. Takes the phase offset samples and disciplines the system clock. Handles loop filtering, frequency correction, and leap seconds.

### Optional PTP path

The IEP peripheral can also be configured as a PTP Hardware Clock (PHC) via the `pru_iep` kernel driver, exposing it as `/dev/ptpN`. This project does **not** use that path — it reads the IEP counter directly via `/dev/mem`. However, the same IEP hardware could support IEEE 1588 PTP timestamping if integrated with `linuxptp` instead of Chrony SHM.

### Deep dives

- [Timestamp Model](timestamp-model.md) — what exactly the PRU captures and what defines t=0
- [Clock Domains](clock-domains.md) — how IEP ticks are correlated with system time, memory barriers
- [Measurements](measurements.md) — expected performance numbers and how to reproduce

[Next: Hardware Requirements](hardware.md)
