# Userspace Daemon & Chrony Configuration

## The reference path

chrony's selected PPS source is the **PHC extts channel**: `ptp_pruss` surfaces each
eCAP capture as a PTP external timestamp, and chrony pairs it with its own
PHC-to-system model. No userspace clock model sits in that path
([architecture](architecture.md)).

```
# GPS coarse time via gpsd/SHM unit 0: second numbering only
refclock SHM 0 refid GPS precision 1e-1 delay 0.3 poll 2 trust noselect

# legacy daemon feed, kept as a comparison witness
refclock SHM 2 refid SHMP precision 1e-9 poll 1 noselect pps lock GPS offset -0.0000047

# the real reference: hardware PPS capture read off the PHC
refclock PHC /dev/ptp1:extpps:pin=-1 refid PPS precision 1e-9 poll 1 prefer trust lock GPS offset -0.0000047

# hardware packet timestamps from the PRU1 ring (rxcomp: trailer-to-SFD, 100 Mbit)
hwtimestamp eth0 rxfilter all rxcomp 7.5e-6 minpoll -4 maxpoll -4 minsamples 8 maxsamples 64

# DS3231 tempco correction, written by the daemon
tempcomp /run/pps-tcxo-ppm 60 0 0 1 0
```

`pin=-1` matters: chrony otherwise issues `PTP_PIN_SETFUNC`, which a pinless driver
rejects and chronyd exits with "Could not enable external PHC timestamping".
`lock GPS` resolves which second each pulse belongs to. `offset` is the measured
antenna plus capture chain delay.

## The daemon: `pru_pps_shm`

With chrony on extts, the daemon is the calibrator and instrument keeper:

1. **Blocks on rpmsg** for each PPS capture from PRU0 (zero CPU between pulses)
2. **Fit A** (ticks vs `CLOCK_MONOTONIC_RAW`, bracketed reads, best of three):
   hardware-to-unsteered-timeline, 11 to 18 ns RMS
3. **Fit B** (`REALTIME` minus `MONOTONIC_RAW`): models chrony's slewing so the SHM
   witness sample can be projected; step detection flushes it on clock steps
4. **qErr**: parses UBX-TIM-TP from gpsd's raw stream and subtracts the receiver's
   per-pulse quantization error, matched by GPS week/tow (`-q 1`)
5. **DS3231 tempco**: counts the PRU's 32 kHz snapshots, learns TCXO error vs GPS
   per temperature bin (persisted to /var/lib), writes `/run/pps-tcxo-ppm` for
   chrony's `tempcomp` and holds the PHC frequency through GPS outages
6. **PHC keeper**: trims the PHC frequency to GPS (~1/min, `clock_adjtime`),
   exports `pruss_phc_offset_ns` (PHC vs GPS at the capture, exact) for the
   [PTP measurement](../measurement/), and Prometheus textfiles in `/run/pruts`
7. **SHM witness**: still writes NTP SHM unit 2 (mode-1 handshake) so the old and
   new reference paths stay comparable in `refclocks.log`

Options: `-s` SHM unit, `-r` rpmsg device, `-q` qErr sign, `-o` PPS chain delay ns
(the same constant as the refclock `offset`, default 4700).

Build and services:

```bash
gcc -O2 -Wall -o /usr/local/bin/pru_pps_shm pru_pps_shm.c -lrt -lm
systemctl enable --now pru-pps-shm      # daemon/pru-pps-shm.service
systemctl enable --now pruts            # kernel/pruts.service: PRU1 fw + modules + PHC at TAI
```

`pru-pps-shm.service` boots PRU0 via remoteproc1 and runs the daemon SCHED_FIFO.
`pruts.service` waits out the remoteproc2 boot race, loads the pktts firmware,
inserts `ptp_pruss` + `cpsw_pruts`, and places the PHC at TAI.

[Next: Verification & Troubleshooting](verification.md) | [Previous: PRU Firmware](firmware.md)
