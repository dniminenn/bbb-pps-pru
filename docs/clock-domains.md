# Clock Domains & Synchronization

## The two clocks

| Property | IEP (PRU) | ARM `CLOCK_REALTIME` |
|----------|-----------|----------------------|
| Frequency | 200 MHz (nominal) | NTP-disciplined, slewed by `chronyd` |
| Resolution | 5 ns/tick | ~1 ns (`clock_gettime`) |
| Width | 32-bit, wraps ~21.5 s | 64-bit `timespec` |
| Source | PRU-ICSS on-chip oscillator | ARM core, same board crystal |
| Disciplined? | No — free-running | Yes — Chrony adjusts via `adjtimex` |

Both clocks derive from the same 24 MHz board crystal, but the IEP is **not** NTP-disciplined. Its effective frequency drifts with temperature. The daemon's IIR filter tracks this drift.

## How PRU time is correlated with system time

On each PPS pulse, immediately after detecting a new `seq`, the daemon runs a **tight calibration loop** ([`pru_pps_shm.c:169-181`](../daemon/pru_pps_shm.c)):

```c
for (i = 0; i < 10; i++) {
    clock_gettime(CLOCK_REALTIME, &t1);
    uint32_t c = iep[0x0C / 4];         // read IEP_COUNT_LO via /dev/mem
    clock_gettime(CLOCK_REALTIME, &t2);

    long long spread = t2 - t1;          // bracket width
    if (spread < best_spread) {
        best_cal_iep  = c;
        best_cal_wall = t1 + spread / 2; // midpoint estimate
    }
}
```

This gives a matched pair `(best_cal_iep, best_cal_wall)` — a known point in time expressed in both clock domains simultaneously. The tightest bracket (smallest `spread`, typically 1–2 µs on an RT kernel) is kept because it minimizes the uncertainty of the wall-time estimate.

### Projection to PPS edge

The PPS edge was latched as `pps_iep` (raw IEP ticks). The daemon projects to wall time:

```
pps_wall_ns = best_cal_wall + (pps_iep - best_cal_iep) × ns_per_tick
```

Since the PPS edge always occurs **before** the calibration read, `(pps_iep - best_cal_iep)` is a small negative offset (interpreted via signed 32-bit cast), typically –20 k ticks (~100 µs).

### ns_per_tick tracking

The IIR-filtered tick rate estimate adapts to thermal drift:

```c
double measured = 1e9 / (double)iep_delta;       // this pulse's ns/tick
ns_per_tick    = ns_per_tick * 0.9 + measured * 0.1;  // slow filter
```

Nominal value is ~4.99971 ns/tick (200,011,500 ticks/s). The 0.9/0.1 filter time constant means it takes ~10 pulses (~10 s) to converge to a new operating point after a temperature step.

## Memory mechanism

The PRU and the ARM share access to **PRU Data RAM 0** at physical address `0x4A300000`. This is ordinary on-chip SRAM — not cacheable on the ARM side when mapped via `/dev/mem` with `O_SYNC`.

| Participant | Access method | Direction |
|-------------|---------------|-----------|
| PRU0 | Direct load/store (address `0x00000000` in PRU local space) | Write |
| Daemon (ARM) | `/dev/mem` mmap at `0x4A300000`, `PROT_READ`, `MAP_SHARED` | Read-only |

The same mechanism is used for the IEP counter registers at `0x4A32E000` — the daemon reads `IEP_COUNT_LO` directly via the mmap'd register page.

No UIO, no rpmsg data channel, no DMA. The rpmsg/vring setup in the firmware exists **only** to satisfy the `remoteproc` driver's requirement — it is needed for the kernel to boot the PRU and mark it as running.

## Ordering guarantees (race-freedom)

### PRU → Daemon (PRU DRAM)

The PRU writes `iep_lo` **before** incrementing `seq`:

```c
pps_data.iep_lo = IEP_COUNT_LO;
pps_data.seq++;
```

The PRU executes in strict program order (no reordering, no cache, no write buffer). From the ARM side, `/dev/mem` with `O_SYNC` bypasses the ARM data cache, so the daemon sees stores in the order the PRU made them. The daemon polls `seq` and only reads `iep_lo` after observing a new `seq` value, guaranteeing it reads the correctly paired timestamp.

### Daemon → Chrony (NTP SHM)

The daemon uses mode-1 count handshake with `__sync_synchronize()` (full memory barrier):

```c
shm->valid = 0;
__sync_synchronize();
shm->count++;              // now odd — "writing"
// ... write all timestamp fields ...
__sync_synchronize();
shm->count++;              // now even — "done"
shm->valid = 1;
```

Chrony reads the SHM segment and checks that `count` did not change between its read of the fields and its re-read of `count`. If `count` changed (writer was active), Chrony discards the sample. The `__sync_synchronize()` barriers ensure the ARM CPU does not reorder stores across the handshake boundaries.

[Next: Measurements](measurements.md) | [Previous: Timestamp Model](timestamp-model.md)
