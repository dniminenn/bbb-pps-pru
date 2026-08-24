# chrony `serveoffset` patch

The CPTS timestamping engine in the AM335x hardware-stamps PTP event frames only, so
chrony's NTP serving on the BBB rides software timestamps regardless of `hwtimestamp`.
That leaves a constant serving bias (kernel rx/tx path asymmetry; measured -11.5 µs on
chron against two independent hardware-timestamped observers) that stock chrony has no
knob to remove: shifting the refclock would move the system clock itself, which also
feeds the CPTS PHC.

This patch (against chrony 4.6.1) adds one directive:

```
serveoffset 0.0000116
```

Seconds, added to the receive and transmit timestamps written into `MODE_SERVER`
replies, basic and interleaved alike. Both timestamps shift together, so client-computed
delay is unchanged. The discipline path, client mode, refclocks, and stored interleaved
state are untouched; the offset is applied only at packet-write time, so it can never
accumulate.

## Build (on the BBB)

```
sudo apt-get install libcap-dev libseccomp-dev nettle-dev pkg-config
tar xzf chrony-4.6.1.tar.gz && cd chrony-4.6.1
patch -p1 < serveoffset.patch
./configure --enable-scfilter --sysconfdir=/etc/chrony
make chronyd
```

Debian's unit runs `chronyd -u root -F 1`, so the build must show `+PRIVDROP +SCFILTER`
in `--version`. Install without fighting the package:

```
sudo dpkg-divert --local --rename --add /usr/sbin/chronyd
sudo install -m 755 chronyd /usr/sbin/chronyd
sudo systemctl restart chrony
```

## Calibration

Measure the served offset from a hardware-timestamped observer on the same switch
(ntpqual), take the multi-day median, and set `serveoffset` to its negation. Re-measure,
trim once. The bias is a property of the kernel path and NIC, so re-derive after kernel
or hardware changes.

## Rollback

```
sudo rm /usr/sbin/chronyd
sudo dpkg-divert --rename --remove /usr/sbin/chronyd
sudo systemctl restart chrony
```

## refclock: PPS via PHC extts

`ptp_pruss` exposes the eCAP PPS capture as extts channel 0, so chrony reads
the hardware pulse on its own PHC-system model:

```
refclock PHC /dev/ptp1:extpps:pin=-1 refid PPS precision 1e-9 poll 1 prefer trust lock GPS offset -0.0000047
```

`pin=-1` skips PTP_PIN_SETFUNC (the driver has no pins). The old SHM 2 feed
from pru_pps_shm stays configured as a noselect witness (refid SHMP).

Why: the SHM receive timestamps were projected through a model of chrony's own
slewing, a feedback loop that rang microseconds under CPU load bursts. The
extts path has no userspace middleman. Measured through a 90 s full-CPU burst:
88 ns rms, worst sample 424 ns, zero samples past 500 ns.
