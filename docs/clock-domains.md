# Clock Domains & Synchronization

## The clocks

| Property | eCAP TSCTR (PRU) | PHC (`/dev/ptpX`) | ARM `CLOCK_REALTIME` |
|----------|-----------------|-------------------|----------------------|
| Rate | 200 MHz nominal | TSCTR, GPS-trimmed | chrony-slewed |
| Width | 32-bit, wraps ~21.5 s | 64-bit ns | 64-bit timespec |
| Disciplined | no | frequency only | yes |

All derive from the same board crystal; the raw counter runs ~53 ppm fast of true
and drifts with temperature. `ptp_pruss` extends the counter and applies a settable
phase base plus a 1 ppb-resolution frequency trim, both folded through the packet
stamp conversion so the PHC and the stamps stay one timescale. The daemon commands
the trim from GPS (fit B slew) or, in holdover, from the DS3231 tempco model.

## The reference crossing (extts)

chrony receives each PPS capture as a PHC-domain event and maintains its own
PHC-to-system samples (`PTP_SYS_OFFSET_EXTENDED`, bracketed inside the driver's
gettime). One servo, one model, no userspace clock arithmetic in the loop.

## The witness crossing (daemon fits)

The SHM witness sample still needs `capture -> CLOCK_REALTIME`. The daemon splits
that into two fits, sampled at 40 Hz and evaluated at each pulse:

- **Fit A: ticks vs `CLOCK_MONOTONIC_RAW`.** Bracketed reads (RAW / TSCTR / RAW,
  best of three, spread-gated) feed a centered least-squares window. Hardware to
  unsteered timeline, no kinks: 11 to 18 ns RMS.
- **Fit B: `REALTIME - MONOTONIC_RAW`.** Models chrony's slewing (vDSO pairs,
  spread-gated, step detection with three-strike flush).

`pps_wall = fitA(capture) + fitB(fitA(capture))`.

Splitting matters because the two error sources differ: A is bus-read noise against
a clean timeline, B is a piecewise-linear servo trajectory. But B is also why the
witness path is only a witness: it models chrony's own steering, which closes a
feedback loop when chrony consumes the result, and under CPU load bursts that loop
rang at microsecond scale. The extts path has no B.

## Ordering guarantees

PRU0 writes the capture value before bumping `seq`, and the rpmsg kick comes last,
so the daemon always reads a consistent `{seq, capture}`. SHM uses the mode-1 count
handshake (odd while writing). All raw 32-bit reads are placed on the extended
timeline by signed deltas against a reference no older than ~10 s.

[Next: Measurements](measurements.md) | [Previous: Timestamp Model](timestamp-model.md)
