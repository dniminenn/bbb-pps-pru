# Timestamp Model

## What is produced?

The eCAP capture unit produces a raw 32-bit TSCTR snapshot latched at the PPS rising
edge, entirely in the 200 MHz (5 ns/tick) PRU clock domain. Everything downstream is
interpretation:

- **extts path (the reference):** `ptp_pruss` extends the snapshot to 64 bits, scales
  it onto the PHC timescale (phase at TAI, frequency GPS-trimmed), and delivers it to
  chrony as an external timestamp event. chrony relates it to system time with its
  own PHC-system model.
- **SHM witness path:** the daemon projects the same capture into `CLOCK_REALTIME`
  via its fits and writes a phase sample (`clockTimeStamp - receiveTimeStamp`) to
  NTP SHM.

## Which edge is captured?

The **rising edge** on P8_15, latched by eCAP CAP1 in silicon
([`main_ecap.c`](../firmware/main_ecap.c)): `ECCTL1 = CAPLDEN, CAP1 rising, absolute`,
`ECCTL2 = run, continuous, wrap after CEVT1`. PRU0 notices the CEVT1 flag after the
fact and forwards `{seq, capture}` over rpmsg; software timing never touches the edge.
The latch-then-bump-seq write order guarantees the daemon sees a consistent pair.

## What defines t = 0?

TSCTR is zeroed once at PRU boot and free-runs, wrapping every ~21.5 s. Raw t=0 has
no meaning; the kernel module's extension plus its settable phase base give the PHC
timescale meaning (`pruts.service` places it at TAI), and the daemon's fit A gives
the witness path its wall-time meaning. All consumers of raw reads funnel through
signed 32-bit deltas against a recent reference, which places any tick within
±10.7 s correctly; both the module worker (4 Hz) and the daemon sampler (40 Hz) are
far inside that.

## One timebase, three consumers

PPS captures (eCAP CAP1), packet stamps (PRU1 ring), and the PHC itself all read the
same TSCTR register. That is the property the whole design leans on: the NTP serving
stamps, the PPS reference, and the PTP measurement are mutually consistent by
construction, with no cross-clock transfer between them.

[Next: Clock Domains](clock-domains.md) | [Previous: Verification & Troubleshooting](verification.md)
