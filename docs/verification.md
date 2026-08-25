# Verification & Troubleshooting

## Verifying the Setup

```bash
# Both PRUs running the right firmware
cat /sys/class/remoteproc/remoteproc1/state /sys/class/remoteproc/remoteproc1/firmware
# running / am335x-pru0-fw
cat /sys/class/remoteproc/remoteproc2/state /sys/class/remoteproc/remoteproc2/firmware
# running / am335x-pru1-fw-pktts

# Modules and clocks
lsmod | grep -E 'ptp_pruss|cpsw_pruts'
grep -H . /sys/class/ptp/ptp*/clock_name      # one of them: pruss-ecap
ethtool -T eth0 | grep 'PTP Hardware Clock'   # points at the pruss PHC

# rpmsg channel
dmesg | grep rpmsg_pru                        # /dev/rpmsg_pru30

# Daemon
journalctl -u pru-pps-shm -f
# seq=... offset=... A[n=... rms=17ns ...] B[...] qerr=... applied [good=... bad=0 ...]
# phc trim -52747 ppb (gps)

# chrony
chronyc sources -v    # PPS selected (*), tens of ns; SHMP and GPS reachable witnesses
chronyc serverstats   # hardware TX/RX timestamp counters increasing while serving
```

Field meanings and healthy ranges: [measurements](measurements.md).

---

## Common Failure Modes

### gpsd silently stops feeding the coarse time, and the clock coasts

Symptom: reference time frozen, root dispersion climbing, the box drifts and looks
like a falseticker from outside, while the PPS capture path is perfectly healthy.

Cause: both PPS refclocks are `lock GPS`; they need gpsd's coarse second (SHM unit
0) to number the pulses. gpsd can stop publishing unit 0 while the process stays
alive with a valid fix, so `Restart=` never triggers, chrony discards the good PPS,
and the clock free-runs.

Diagnose: `sudo ntpshmmon` (NTP0 silent), `chronyc -n sources` (GPS reach 0).
Fix now: `sudo systemctl restart gpsd gpsd.socket`.

Prevent: an output-liveness watchdog
([`tools/gps-refclock-watchdog.sh`](../tools/gps-refclock-watchdog.sh), timer every
2 min) restarts gpsd when the GPS
refclock reads reach 0 twice, 12 s apart. Two guards matter: the double read rides
out the normal reach rebuild, and the script exits quietly while chronyd or gpsd is
under 3 minutes old, because judging a freshly restarted chronyd restarts a healthy
gpsd, which costs ~2 min of NTP0 silence and rings the clock. Unattended recovery
lands in 1 to 3 minutes.

### Keep the extts refclock selected

The SHM witness must stay `noselect`: consuming it closes the fit-B feedback loop
([clock domains](clock-domains.md)) and CPU load bursts ring the clock at µs scale.
The extts reference holds ~90 ns RMS through 100% CPU.

---

## Remoteproc Map

| remoteproc | PRU | Physical | Firmware |
|------------|-----|----------|----------|
| remoteproc0 | PM | N/A | am335x-pm-firmware.elf |
| remoteproc1 | PRU0 | 0x4a334000 | am335x-pru0-fw |
| remoteproc2 | PRU1 | 0x4a338000 | am335x-pru1-fw-pktts |

PRU DRAM0 base `0x4A300000`, ICSS eCAP base `0x4A330000` (both mapped read-only by
the daemon).

---

## Updating Firmware, Modules, Daemon

Build steps: [firmware](firmware.md), `make` in [`kernel/`](../kernel/), one gcc line
for the daemon. Deploy order matters because chrony and the daemon hold the PHC and
the daemon holds rpmsg:

```bash
systemctl stop ptp-prom ptp4l pru-pps-shm chrony
systemctl restart pruts        # rmmod/insmod + PRU1 fw + PHC at TAI
systemctl start chrony pru-pps-shm ptp4l ptp-prom
```

On this kernel, runtime remoteproc stop/bind writes are silently ignored, so **new
PRU firmware needs a reboot**; everything is enabled and comes up in order.

---

## Troubleshooting

**PRU stays offline**: firmware file present in /lib/firmware, both rproc overlays
active, `echo start > /sys/class/remoteproc/remoteprocN/state`.

**No `/dev/rpmsg_pru30`**: PRU-RPROC-VRING overlay missing, or firmware failed
before rpmsg init.

**seq never increments**: PPS not reaching P8_15 (scope it), eCAP overlay not
loaded, or the receiver has no fix yet.

**chronyd exits: "Could not enable external PHC timestamping"**: the refclock line
is missing `:pin=-1` (chrony insists on PTP_PIN_SETFUNC otherwise, the driver has
no pins).

**`bad` counter rising**: pulse spacing outside sanity (wiring, GPS cold start).

**Interleaved clients see µs outliers under heavy CPU load**: known artifact class
of the serving path under abuse load; the clock itself stays clean (verify via the
witness refids in refclocks.log).

[Previous: Userspace Daemon & Chrony](daemon.md)
