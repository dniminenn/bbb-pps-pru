# Measurements & Validation

Internal precision (how tightly the box holds its own PPS) and external accuracy
(what independent observers measure) are different claims; both are below.

## Measured (2026-08-25)

| Metric | Value |
|--------|-------|
| Fit A residuals (ticks vs raw timeline) | 11 to 18 ns RMS |
| chrony tracking RMS vs PPS | 64 ns |
| Served NTP, 1 h gated mean, observer A / B | -126 / -152 ns |
| Served NTP, 1 h gated p95 | 393 / 449 ns |
| Served NTP, 1 h gated stddev | 548 / 289 ns |
| extts refclock through 90 s of 100% CPU | 88 ns RMS, max 424 ns |
| PTP read of the elected grandmaster | within ~0.5 µs |

The served-time rows are ntpqual measurements: hardware-timestamped interleaved NTP,
delay-gated, taken continuously by two independent GPS-disciplined observers on
separate switches. The two observers agree with each other at the 25 ns level, which
bounds the measurement itself; the box is never graded by its own telemetry alone.

## Interpreting daemon output

```
seq=28644 offset=-4482 A[n=348 rms=17ns slope=5.000000] B[n=320 rms=106ns slew=-52.762ppm] qerr=-3.7 applied [good=23411 bad=0 dropped=2]
```

| Field | Meaning | Healthy |
|-------|---------|---------|
| `offset` | raw pulse offset, includes the ~4.7 µs chain delay | steady around the calibrated constant |
| `A rms/slope` | fit A residual, ns per tick | < 25 ns, slope ~5.000000 |
| `B rms/slew` | fit B residual and chrony's slew | < 200 ns, slew ≈ crystal error |
| `qerr` | receiver quantization correction | ±10 ns, `applied` |
| `good/bad/dropped` | pulse accounting | `bad` 0 |

Also logged: `phc trim <ppb> (gps|ds3231)` about once a minute, and
`tcxo: timebase ... vs DS3231` every ~60 s from the tempco learner.

## Reproducing

Follow [setup](setup.md), [firmware](firmware.md), [daemon](daemon.md); then:

```bash
chronyc sources         # PPS selected (*), SHMP and GPS as witnesses
chronyc tracking        # RMS offset tens of ns after convergence
journalctl -u pru-pps-shm -f
tools/parse_offsets.sh  # statistics over daemon logs
```

For external verdicts, measure from another machine with hardware timestamping and
interleaved NTP; a software-timestamped client measures its own NIC, not this clock.

[Previous: Clock Domains](clock-domains.md)
