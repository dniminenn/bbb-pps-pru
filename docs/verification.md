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

- `delta`: IEP ticks between consecutive PPS pulses (~200,011,500 for 200 MHz IEP with slight trim)
- `offset`: sub-second residual of the detected edge (should be <1 µs steady-state)
- `gap`: IEP ticks between PPS edge and calibration sample (how stale our cal is)
- `spread`: nanoseconds between the two bracketing `clock_gettime` calls (quality of wall cal)

### Check Chrony

```bash
chronyc sources -v
# PPS source should show * (selected) with offset in tens of nanoseconds
```

---

## Common Failure Modes

### gpsd silently stops feeding the coarse time, and the clock coasts

Symptom: `chronyc tracking` shows the reference time frozen minutes or hours in the
past, root dispersion climbing into the milliseconds, and the system clock slowly
drifting. From another host this server looks like a falseticker and gets dropped.
The PRU PPS daemon looks completely healthy the whole time.

Cause: the PPS refclock is `lock GPS`, so it needs the coarse second from gpsd
(SHM unit 0, refid `GPS`) to know which second each pulse belongs to. gpsd can stop
publishing SHM unit 0 while the gpsd process stays alive and still holds a fix, so a
normal systemd `Restart=` policy never triggers. With unit 0 dead, chrony discards
the good PPS and free-runs.

Diagnose:

```bash
# Only NTP2 (PRU PPS) updates; NTP0 (gpsd coarse) is silent:
sudo ntpshmmon

# The GPS refclock shows reach 0 with an old last-sample time:
chronyc -n sources        # look at the "GPS" line, Reach column
```

Fix now:

```bash
sudo systemctl restart gpsd gpsd.socket   # chrony relocks PPS within a few polls
```

Prevent it (auto-recovery): a small output-liveness watchdog restarts gpsd when the
GPS refclock actually goes stale. SHM unit 0 is written only by gpsd, so a reach-0
`GPS` refclock is unambiguously a gpsd fault. The double read 12 s apart avoids
false-firing during the normal reach rebuild right after gpsd starts.

`/usr/local/sbin/gps-refclock-watchdog.sh`:

```bash
#!/usr/bin/env bash
set -u
gps_reach() { chronyc -n sources 2>/dev/null | awk '$2=="GPS"{print $5}'; }
r1=$(gps_reach); [ "${r1:-x}" = "0" ] || exit 0   # healthy, rebuilding, or chrony down
sleep 12
r2=$(gps_reach); [ "${r2:-x}" = "0" ] || exit 0   # cleared on its own (post-restart blip)
logger -t gps-refclock-watchdog "GPS refclock (gpsd NTP0) reach=0 for >12s; restarting gpsd"
systemctl restart gpsd gpsd.socket
```

Run it every 2 minutes with a systemd timer:

```ini
# /etc/systemd/system/gps-refclock-watchdog.service
[Unit]
Description=Restart gpsd if the chrony GPS refclock (SHM NTP0) goes stale
After=chrony.service gpsd.service
[Service]
Type=oneshot
ExecStart=/usr/local/sbin/gps-refclock-watchdog.sh

# /etc/systemd/system/gps-refclock-watchdog.timer
[Unit]
Description=Run the GPS refclock watchdog every 2 minutes
[Timer]
OnBootSec=3min
OnUnitActiveSec=2min
AccuracySec=10s
[Install]
WantedBy=timers.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now gps-refclock-watchdog.timer
```

Unattended recovery lands in 1 to 3 minutes (time for reach to decay to 0 plus one
timer interval), which chrony rides out on holdover.

---

## Remoteproc Map

| remoteproc | PRU | Physical | Firmware |
|------------|-----|----------|----------|
| remoteproc0 | PM firmware | N/A | am335x-pm-firmware.elf |
| remoteproc1 | PRU0 | 0x4a334000 | am335x-pru0-fw |
| remoteproc2 | PRU1 | 0x4a338000 | am335x-pru1-fw |

PRU DRAM0 base: `0x4A300000` (mapped read-only by pru_pps_shm)  
IEP base: `0x4A32E000` (mapped read-only for live counter reads during calibration)

---

## Updating Firmware & Daemon

When you've made code changes and need to redeploy on a running system:

```bash
# 1. Stop the daemon
systemctl stop pru-pps-shm

# 2. Stop the PRU
echo stop > /sys/class/remoteproc/remoteproc1/state

# 3. Rebuild firmware (from the firmware/ directory)
export PRU_CGT=/usr/share/ti/cgt-pru
PSSP=/opt/source/pssp
rm -f gen/*

$PRU_CGT/bin/clpru --silicon_version=3 -O2 \
  --include_path=$PRU_CGT/include \
  --include_path=$PSSP/include \
  --include_path=$PSSP/include/am335x \
  --display_error_number --endian=little --hardware_mac=on \
  --obj_directory=gen --pp_directory=gen -ppd -ppa \
  -fe gen/main.obj main.c && \
$PRU_CGT/bin/clpru --silicon_version=3 -O2 \
  --display_error_number --endian=little --hardware_mac=on \
  -z -i$PRU_CGT/lib -i$PRU_CGT/include \
  --reread_libs --warn_sections --stack_size=0x100 --heap_size=0x100 \
  -o gen/pru-pps.out gen/main.obj \
  -m gen/pru-pps.map \
  ./AM335x_PRU_intc_rscTbl.cmd \
  --library=libc.a \
  --library=$PSSP/lib/rpmsg_lib.lib && \
echo "PRU BUILD OK"

# 4. Install new firmware
cp gen/pru-pps.out /lib/firmware/am335x-pru0-fw

# 5. Rebuild daemon (from the daemon/ directory)
gcc -O2 -Wall -o /usr/local/bin/pru_pps_shm pru_pps_shm.c -lrt -lm

# 6. Boot the PRU and start the daemon
echo start > /sys/class/remoteproc/remoteproc1/state
sleep 3
systemctl start pru-pps-shm

# 7. Verify
journalctl -u pru-pps-shm -f
```

> **Important:** The PRU must be stopped before replacing the firmware binary, and the daemon must be stopped before the PRU, otherwise the daemon may hold a stale mmap or rpmsg fd. The sleep between PRU start and daemon start gives the rpmsg channel time to initialize.

---

## Troubleshooting

**PRU stays offline / firmware not loading**
- Confirm `am335x-pru0-fw` exists in `/lib/firmware/` and is the correct `.out` binary
- Check `PRU-RPROC-VRING-00A0.dtbo` and `AM335X-PRU-RPROC-4-19-TI-00A0.dtbo` are both active
- Try manually: `echo start > /sys/class/remoteproc/remoteproc1/state`

**No rpmsg channel / `/dev/rpmsg_pru30` missing**
- The firmware must initialize rpmsg and call `pru_rpmsg_channel()` before the channel appears
- `PRU-RPROC-VRING-00A0.dtbo` must be loaded (it wires the interrupt lines)

**seq never increments**
- Confirm P8_16 pinmux is correct: `0x38 0x36` (mode 6, input enabled, pull disabled)
- Verify PPS signal present on P8_16 with a multimeter or scope
- Check `PRU-PPS-PINMUX-00A0.dtbo` is loaded at boot

**Large offsets or bad samples**
- `bad` counter increments when `iep_delta` is outside 150M–250M ticks (sanity check for 1 Hz)
- Large `gap` values (>500 µs) mean the daemon didn't get scheduled quickly after the edge: check `CPUSchedulingPolicy=fifo` is effective (`sched_setscheduler` succeeds)
- Thermal drift: IEP frequency shifts with temperature; the IIR filter adapts but needs a few minutes to settle

**PRU won't stop / `echo stop` has no effect**
- `echo stop > /sys/class/remoteproc/remoteproc1/state` may silently fail when the PRU is in a tight polling loop
- Ensure the daemon is stopped first (`systemctl stop pru-pps-shm`) and no process has `/dev/rpmsg_pru30` open (`lsof /dev/rpmsg_pru30`)
- If the PRU still won't stop, a **reboot is the most reliable way** to reload firmware. Since the firmware is at `/lib/firmware/am335x-pru0-fw` and the service is enabled, everything will come up cleanly after reboot

[Previous: Userspace Daemon & Chrony](daemon.md)
