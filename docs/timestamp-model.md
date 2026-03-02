# Timestamp Model

## What is produced?

The PRU firmware does **not** produce wall-clock time. It produces a single raw 32-bit IEP counter snapshot (`iep_lo`) captured at the instant the PPS rising edge is detected. This value lives entirely in the IEP clock domain (200 MHz, 5 ns/tick). The conversion to wall time happens later, in the userspace daemon.

The final output written to NTP SHM is a **phase measurement**: `clockTimeStamp - receiveTimeStamp`, where `clockTimeStamp` is the ideal UTC second boundary and `receiveTimeStamp` is the daemon's best estimate of the wall-clock time at the PPS edge. Chrony treats this difference as a signed phase offset and uses it to discipline the system clock.

## Which edge is captured?

The **rising edge** of the PPS signal on P8_16 is captured.

The PRU polls `__R31` bit 14 in a tight loop. A rising edge is detected when the current sample is high and the previous sample was low ([`main.c:74`](../firmware/main.c)):

```c
if (cur && !prev) {
    pps_data.iep_lo = IEP_COUNT_LO;   // latch raw IEP ticks
    pps_data.seq++;                    // signal new edge to daemon
}
```

The latch-then-increment order means `iep_lo` is always written **before** `seq` is bumped, which is the ordering the daemon relies on.

## What defines t = 0?

The IEP counter is zeroed and started once during PRU boot in [`iep_init()`](../firmware/main.c):

```c
IEP_GLOBAL_CFG = 0;       // disable
IEP_COUNT_LO   = 0;       // zero
IEP_COMPEN     = 0;        // no compensation
IEP_GLOBAL_CFG = 0x11;    // enable, increment by 1 each 5-ns cycle
```

After this the counter free-runs and wraps at 2³² ticks (~21.5 seconds). **No absolute time meaning is assigned to t = 0** — only deltas matter. The daemon correlates IEP ticks with `CLOCK_REALTIME` at every PPS edge via the calibration loop.

## Clock domain

The raw timestamp lives in the **IEP clock domain**: a 200 MHz free-running counter internal to the PRU-ICSS. It has no direct relationship to `CLOCK_REALTIME` or any NTP-disciplined clock.

The daemon's calibration loop ([`pru_pps_shm.c:169-181`](../daemon/pru_pps_shm.c)) performs the domain crossing on every PPS pulse — see [Clock Domains](clock-domains.md) for the full explanation.

## 32-bit wrap handling

`IEP_COUNT_LO` is 32 bits wide and wraps every ~21.5 s at 200 MHz. Since PPS pulses arrive once per second, the maximum IEP delta between consecutive pulses is ~200 M ticks — well within the 32-bit range. The daemon uses unsigned subtraction, which handles a single wrap correctly even if `pps_iep < prev_pps_iep`.

The calibration gap (time between PPS edge and calibration read) is typically < 100 µs (~20 k ticks), so IEP wrap between the PPS latch and the calibration read is not a concern in practice.

[Next: Clock Domains](clock-domains.md) | [Previous: Verification & Troubleshooting](verification.md)
