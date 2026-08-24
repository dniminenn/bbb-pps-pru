# Measurement

PTP cross-validation of the LAN's grandmasters from this box, without serving
PTP and without touching any clock.

- `ptp4l.service` runs ptp4l as a free-running, client-only instance on the
  pruss PHC. The PRU stamps every frame on the wire, so PTP event messages get
  the same hardware timestamps NTP serving uses. Nothing is disciplined.
- `ptp-prom` + `ptp-prom.service` poll ptp4l over its UDS and publish
  `ptp_offset_from_master_ns` and `ptp_mean_path_delay_ns` into the pruts
  textfile dir for node_exporter.
- `pru_pps_shm` exports `pruss_phc_offset_ns`, the PHC's error against GPS
  (TAI), anchored to the hardware PPS capture. The grandmaster's error against
  this box's GPS is then simply:

      gm_minus_gps = pruss_phc_offset_ns - ptp_offset_from_master_ns

Both grandmasters sit in PTP domain 0, so BMCA elects one and that is the one
measured, the same time any PTP consumer on this LAN would receive. The `gm`
label carries the elected clock identity.

Stamp conventions: the PRU captures frames at the trailer, i210 grandmasters
stamp at the SFD, and at this box's 100 Mbit port one PTP event frame is
~8.4 us on the wire. ptp4l.conf carries `ingressLatency 8400` /
`egressLatency -8400` to move the PRU stamps onto the SFD reference plane
(the NTP-side equivalent is chrony's rxcomp). The daemon's `-o` option is the
PPS antenna/capture chain delay, the same constant chrony.conf compensates on
the SHM refclock. With all of that in place the instrument reads the elected
GM within ~0.5 us; the residual few hundred ns is an uncalibrated slice of
the frame-length constant, not clock disagreement (the hw-stamped NTP mesh
puts the true agreement under 100 ns).

An earlier CPTS-based method (phc2sys mirroring the system clock into the CPTS
PHC) lived here; it carried a multi-microsecond noise floor from software PHC
reads and is retired. The original experiment it supported is written up in
[CROSS-VALIDATION.md](../CROSS-VALIDATION.md).
