# Measurements & Validation

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
seq=1234 delta=200011550 offset=+352 ns gap=18354 (91.8 us) spread=1291 ns ns/tick=4.999711 [good=3411]
```

| Field | Meaning | Healthy range |
|-------|---------|---------------|
| `delta` | IEP ticks between consecutive PPS pulses | 199.5 M – 200.5 M |
| `offset` | Sub-second residual of the PPS edge vs UTC second | ±1 µs steady-state |
| `gap` | IEP ticks between PPS edge and calibration sample | < 100 µs (< 20 k ticks) |
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

1. **Daemon running and stable** — `good` counter increasing, `bad` stays at 0
2. **Offsets within range** — Should settle to < 1 µs after a few minutes of IIR convergence
3. **Chrony selected** — `chronyc sources` shows `*` (selected) next to the PPS refclock
4. **Chrony offset** — `chronyc tracking` shows `System time` offset in the ns to low-µs range
5. **No large gap values** — `gap` consistently < 100 µs confirms `SCHED_FIFO` is effective

## What to look for in bad setups

| Symptom | Likely cause |
|---------|--------------|
| `offset` drifts > 10 µs | IIR hasn't converged, or thermal shock — wait 30 s |
| `spread` > 5 µs | Not running RT kernel, or heavy system load |
| `bad` counter > 0 | Missing PPS pulses, loose wiring, or GPS cold start |
| `gap` > 500 µs | Daemon not scheduled promptly — check `SCHED_FIFO` |
| Chrony shows `?` not `*` | SHM unit mismatch, or GPS refclock (`lock`) not valid |

[Previous: Clock Domains](clock-domains.md)
