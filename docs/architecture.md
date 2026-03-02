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

[Next: Hardware Requirements](hardware.md)
