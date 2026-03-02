# Verification & Troubleshooting

## Verifying the Setup

### Check remoteproc state

```bash
cat /sys/class/remoteproc/remoteproc1/state   # should be: running
cat /sys/class/remoteproc/remoteproc1/firmware # should be: am335x-pru0-fw
```

### Check rpmsg channel created

```bash
dmesg | grep -i pru
# remoteproc remoteproc1: Booting fw image am335x-pru0-fw, size 73268
# virtio_rpmsg_bus virtio0: creating channel rpmsg-pru addr 0x1e
# rpmsg_pru virtio0.rpmsg-pru.-1.30: new rpmsg_pru device: /dev/rpmsg_pru30
```

### Check daemon output

```bash
journalctl -u pru-pps-shm -f
```

Expected output:
```
pru_pps_shm: polling PRU DRAM -> SHM unit 2
seq=1234 delta=200011550 offset=+352 ns gap=18354 (91.8 us) spread=1291 ns ns/tick=4.999711 [good=3411]
```

- `delta` — IEP ticks between consecutive PPS pulses (~200,011,500 for 200 MHz IEP with slight trim)
- `offset` — sub-second residual of the detected edge (should be <1 µs steady-state)
- `gap` — IEP ticks between PPS edge and calibration sample (how stale our cal is)
- `spread` — nanoseconds between the two bracketing `clock_gettime` calls (quality of wall cal)

### Check Chrony

```bash
chronyc sources -v
# PPS source should show * (selected) with offset in tens of nanoseconds
```

---

## Remoteproc Map

| remoteproc | PRU | Physical | Firmware |
|------------|-----|----------|----------|
| remoteproc0 | PM firmware | — | am335x-pm-firmware.elf |
| remoteproc1 | PRU0 | 0x4a334000 | am335x-pru0-fw |
| remoteproc2 | PRU1 | 0x4a338000 | am335x-pru1-fw |

PRU DRAM0 base: `0x4A300000` (mapped read-only by pru_pps_shm)  
IEP base: `0x4A32E000` (mapped read-only for live counter reads during calibration)

---

## Troubleshooting

**PRU stays offline / firmware not loading**
- Confirm `am335x-pru0-fw` exists in `/lib/firmware/` and is the correct `.out` binary
- Check `PRU-RPROC-VRING-00A0.dtbo` and `AM335X-PRU-RPROC-4-19-TI-00A0.dtbo` are both active
- Try manually: `echo start > /sys/class/remoteproc/remoteproc1/state`

**No rpmsg channel / `/dev/rpmsg_pru30` missing**
- The firmware must initialize rpmsg and call `pru_rpmsg_channel()` before the channel appears
- `PRU-RPROC-VRING-00A0.dtbo` must be loaded — it wires the interrupt lines

**seq never increments**
- Confirm P8_16 pinmux is correct: `0x38 0x36` (mode 6, input enabled, pull disabled)
- Verify PPS signal present on P8_16 with a multimeter or scope
- Check `PRU-PPS-PINMUX-00A0.dtbo` is loaded at boot

**Large offsets or bad samples**
- `bad` counter increments when `iep_delta` is outside 150M–250M ticks (sanity check for 1 Hz)
- Large `gap` values (>500 µs) mean the daemon didn't get scheduled quickly after the edge — check `CPUSchedulingPolicy=fifo` is effective (`sched_setscheduler` succeeds)
- Thermal drift: IEP frequency shifts with temperature; the IIR filter adapts but needs a few minutes to settle

[Previous: Userspace Daemon & Chrony](daemon.md)
