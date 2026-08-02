# Measurements & Validation

This document covers the clock's **internal precision**: how tightly it holds against
its own PPS and servo (offset, jitter, tracking stats). For **external accuracy**, how
closely it agrees with a separate, independent GPS reference over PTP, see
[CROSS-VALIDATION.md](../CROSS-VALIDATION.md).

## Expected performance

| Metric | PRU PPS (this project) | GPIO PPS (`pps-gpio`) |
|--------|------------------------|------------------------|
| Typical offset | 100–800 ns | 5–20 µs |
| Worst-case outliers | < 2 µs | 10–50 µs |
| Calibration spread | ~1.3 µs | N/A (kernel timestamp) |
| Clock precision | 5 ns (IEP tick) | ~1 µs (GPIO IRQ jitter) |

## Interpreting daemon output

The daemon log line contains all the key health metrics:

```
seq=202 delta=200011758 offset=+634 ns gap=38925 (194.6 us) spread=1291 ns ns/tick=4.999707 [good=201]
```

| Field | Meaning | Healthy range |
|-------|---------|---------------|
| `delta` | IEP ticks between consecutive PPS pulses | 199.5 M – 200.5 M |
| `offset` | Sub-second residual of the PPS edge vs UTC second | ±1 µs steady-state |
| `gap` | IEP ticks between PPS edge and calibration sample | < 250 µs |
| `spread` | Wall-time bracket width of the best calibration pair | < 2 µs on RT kernel |
| `ns/tick` | Filtered IEP tick period | ~4.99971 (200 MHz nom.) |
| `good` / `bad` | Accepted / rejected pulse count | `bad` should be 0 |

## How to reproduce

### Prerequisites

- BeagleBone Black with RT kernel (`6.6.58-ti-rt-arm32` or similar)
- GPS with PPS output wired to P8_16
- PRU firmware loaded, daemon running (see [setup guide](setup.md))
- Chrony configured with `refclock SHM 2` (see [daemon guide](daemon.md))

### Collecting Chrony tracking data

```bash
# Sample chrony tracking every second for 1 hour
chronyc -c tracking | tee chrony_tracking_$(date +%s).csv &

# Or use the sourcestats view
watch -n 10 'chronyc sourcestats'
```

### Collecting daemon logs

```bash
# Capture daemon output for analysis
journalctl -u pru-pps-shm --since "now" -f | tee pru_pps_log_$(date +%s).txt
```

### Parsing offset data

Use the included helper script to extract offset values from daemon logs:

```bash
# Extract offset values and compute statistics
tools/parse_offsets.sh pru_pps_log_*.txt
```

## Validation checklist

1. **Daemon running and stable**: `good` counter increasing, `bad` stays at 0
2. **Offsets within range**: Should settle to < 1 µs after a few minutes of IIR convergence
3. **Chrony selected**: `chronyc sources` shows `*` (selected) next to the PPS refclock
4. **Chrony offset**: `chronyc tracking` shows `System time` offset in the ns to low-µs range
5. **No large gap values**: `gap` consistently < 250 µs confirms the blocking `rpmsg` wakeup and `SCHED_FIFO` are effective

## What to look for in bad setups

| Symptom | Likely cause |
|---------|--------------|
| `offset` drifts > 10 µs | IIR hasn't converged, or thermal shock (wait 30 s) |
| `spread` > 5 µs | Not running RT kernel, or heavy system load |
| `bad` counter > 0 | Missing PPS pulses, loose wiring, or GPS cold start |
| `gap` > 500 µs | Daemon not scheduled promptly (check `SCHED_FIFO`) |
| Chrony shows `?` not `*` | SHM unit mismatch, or GPS refclock (`lock`) not valid |

[Previous: Clock Domains](clock-domains.md)

## Real-World Performance

To demonstrate the stability and precision of this PRU-based PPS capture, brief measurements were collected reflecting the system's performance on the BeagleBone Black hardware running the RT kernel.

### Daemon Offsets
The userspace daemon logs show typical sub-microsecond residuals against the system clock, with `gap` metrics firmly contained within deterministic limits and sub-2µs wall time `spread` measurements:

```
Mar 02 12:27:07 bbb pru_pps_shm[884]: seq=202 delta=200011758 offset=+634 ns gap=38925 (194.6 us) spread=1291 ns ns/tick=4.999707 [good=201]
Mar 02 12:27:07 bbb pru_pps_shm[884]: seq=212 delta=200011735 offset=+658 ns gap=38807 (194.0 us) spread=1291 ns ns/tick=4.999707 [good=211]
Mar 02 12:27:07 bbb pru_pps_shm[884]: seq=222 delta=200011735 offset=+618 ns gap=38102 (190.5 us) spread=1292 ns ns/tick=4.999707 [good=221]
Mar 02 12:27:07 bbb pru_pps_shm[884]: seq=232 delta=200011735 offset=+491 ns gap=39889 (199.4 us) spread=1291 ns ns/tick=4.999707 [good=231]
Mar 02 12:27:07 bbb pru_pps_shm[884]: seq=242 delta=200011810 offset=+924 ns gap=39081 (195.4 us) spread=1291 ns ns/tick=4.999707 [good=241]
Mar 02 12:27:07 bbb pru_pps_shm[884]: seq=252 delta=200011792 offset=+609 ns gap=40515 (202.6 us) spread=1292 ns ns/tick=4.999707 [good=251]
Mar 02 12:27:07 bbb pru_pps_shm[884]: seq=262 delta=200011717 offset=+271 ns gap=40166 (200.8 us) spread=1291 ns ns/tick=4.999707 [good=261]
Mar 02 12:27:07 bbb pru_pps_shm[884]: seq=272 delta=200011667 offset=+177 ns gap=44776 (223.9 us) spread=1291 ns ns/tick=4.999707 [good=271]
Mar 02 12:27:07 bbb pru_pps_shm[884]: seq=282 delta=200011735 offset=+801 ns gap=39961 (199.8 us) spread=1292 ns ns/tick=4.999707 [good=281]
```

### Chrony Statistics & Tracking
For validation, `chronyc` statistics and tracking data have been logged to demonstrate convergence and long-term stability:

- [**Chrony Tracking Data**](chrony-measurements/tracking.txt): Shows the measured offset settling into the low nanosecond domain over time once the servo converges.
- [**Chrony Sourcestats**](chrony-measurements/statistics.txt): Shows an estimated standard deviation converging to `1.0e-09` (1 nanosecond), suggesting the timing jitter introduced by the PRU pathway is minimal compared to standard GPIO IRQs.

An excerpt from the tracking log:
```
===================================================================================================================================
   Date (UTC) Time     IP Address   St   Freq ppm   Skew ppm     Offset L Co  Offset sd Rem. corr. Root delay Root disp. Max. error
===================================================================================================================================
2026-03-02 15:59:44 PPS              1     58.491      0.000  2.234e-11 N  1  1.931e-11  4.910e-11  1.000e-09  2.153e-06  4.728e-06
2026-03-02 15:59:46 PPS              1     58.491      0.000  4.151e-11 N  1  1.450e-17  4.151e-11  1.000e-09  2.496e-06  4.155e-06
2026-03-02 15:59:48 PPS              1     58.491      0.000  1.172e-14 N  1  1.716e-17  1.174e-14  1.000e-09  1.905e-06  4.498e-06
2026-03-02 15:59:50 PPS              1     58.491      0.000  2.141e-15 N  1  8.552e-18  2.135e-15  1.000e-09  2.484e-06  3.907e-06
2026-03-02 15:59:52 PPS              1     58.491      0.000  4.945e-16 N  1  8.621e-18  4.774e-16  1.000e-09  1.920e-06  4.485e-06
2026-03-02 15:59:54 PPS              1     58.491      0.000  1.077e-16 N  1  1.080e-17  9.066e-17  1.000e-09  1.990e-06  3.921e-06
```
## 2026-08 Pipeline Update

The daemon and firmware were reworked in August 2026; the sections above describe the
original single-sandwich pipeline and are kept for comparison. Three error sources were
found and removed, each hiding under the previous one:

1. **Edge-detect jitter in the PRU (~230 ns, white).** The hot loop drained rpmsg
   whenever the host-kick line was asserted, and the kick sysevent was never cleared,
   so after the first kick every polling iteration paid a variable vring scan before
   re-checking the pin. Fixed by clearing the sysevent and servicing rpmsg only right
   after a latch, when the next edge is a second away. Pulse-to-pulse jitter dropped
   from ~225 ns to ~42 ns RMS.

2. **Transfer noise in the daemon (~250 ns per pulse).** One min-spread calibration
   sandwich per pulse was replaced with continuous sampling and a least-squares
   IEP-to-wall fit. Two hard-won constraints: the window must be *centered* on the
   pulse (a trailing window extrapolates across chrony's slope adjustments and the
   daemon-chrony feedback loop rings at hundreds of ns), and it must stay small
   (holding samples ±2 s put enough delay in chrony's servo loop to make it visibly
   hunt; ±0.5 s is stable). Fit residuals: ~8 ns RMS.

3. **Receiver quantization sawtooth (±10 ns).** UBX-TIM-TP `qErr` is parsed from the
   raw stream (as a read-only gpsd client) and matched to its pulse by GPS week/tow
   converted to the UTC second. The sign convention was determined empirically:
   regressing measured pulse offsets against qErr over 900 quiet samples gave a slope
   of -1.4 ±0.35 (4.1 sigma), so the correction is additive. Enabled with `-q 1`.

Because the daemon now labels the absolute UTC second itself, the chrony refclock is a
plain SHM source; the `pps` and `lock` options must be dropped (samples arrive ~0.5 s
after the edge, which pps-pairing rejects but plain SHM handles fine):

```
refclock SHM 2 refid PPS precision 1e-9 poll 2 filter 4 prefer trust
```

Steady state after the rework, ~6 h uptime:

```
Aug 02 11:44:51 bbb pru_pps_shm[3998]: seq=23160 offset=-995 ns fit[n=46 rms=8ns slope=4.999733] qerr=-4.1 applied [good=17381 bad=0 dropped=0]
Aug 02 11:45:01 bbb pru_pps_shm[3998]: seq=23170 offset=-1537 ns fit[n=47 rms=8ns slope=4.999733] qerr=+9.1 applied [good=17391 bad=0 dropped=0]
Aug 02 11:45:11 bbb pru_pps_shm[3998]: seq=23180 offset=-1629 ns fit[n=46 rms=9ns slope=4.999733] qerr=+3.4 applied [good=17401 bad=0 dropped=0]
Aug 02 11:45:21 bbb pru_pps_shm[3998]: seq=23190 offset=-1690 ns fit[n=46 rms=7ns slope=4.999733] qerr=-3.4 applied [good=17411 bad=0 dropped=0]
```

```
Name/IP Address            NP  NR  Span  Frequency  Freq Skew  Offset  Std Dev
==============================================================================
PPS                        10   5    36     -0.000      0.001     -0ns     4ns
```

One operational note: the committed firmware now builds and boots as-is (the
`.pru_irq_map` DATA_SECTION pragma and include order were fixed). On the 6.6-ti kernel
used here, runtime remoteproc stop/bind writes are silently ignored, so loading new PRU
firmware requires a reboot.
