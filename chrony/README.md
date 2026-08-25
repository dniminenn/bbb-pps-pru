# chrony on this box

Two small patches (against chrony master) and the refclock configuration.

## Patches

- [`hwts-ifindex-fallback.patch`](hwts-ifindex-fallback.patch): fall back to the
  message's pktinfo if_index when the timestamping if_index is 0, so hardware RX
  timestamps survive VLAN delivery paths. Submitted upstream (chrony-dev); a kernel
  fix is on netdev.
- [`serveoffset.patch`](serveoffset.patch): a `serveoffset` directive that shifts the
  RX and TX timestamps written into server replies together (client-computed delay
  unchanged), for calibrating a constant serving bias without touching the
  disciplined clock. With the PRU hardware packet timestamps this box runs
  `serveoffset 0`; the knob exists for software-stamped setups.

Build:

```
apt-get install libcap-dev libseccomp-dev nettle-dev pkg-config
patch -p1 < serveoffset.patch
patch -p1 < hwts-ifindex-fallback.patch
./configure --enable-scfilter --sysconfdir=/etc/chrony
make chronyd
```

## refclock: PPS via PHC extts

`ptp_pruss` exposes the eCAP PPS capture as extts channel 0, so chrony reads
the hardware pulse on its own PHC-system model:

```
refclock PHC /dev/ptp1:extpps:pin=-1 refid PPS precision 1e-9 poll 1 prefer trust lock GPS offset -0.0000047
```

`pin=-1` skips PTP_PIN_SETFUNC (the driver has no pins). The daemon's SHM feed
stays configured as a noselect witness (refid SHMP); consuming it would close the
fit-B feedback loop ([docs/clock-domains.md](../docs/clock-domains.md)). Through a
90 s full-CPU test the extts path holds 88 ns RMS, worst sample 424 ns.

Full configuration, including the `hwtimestamp` and `tempcomp` lines:
[docs/daemon.md](../docs/daemon.md).
