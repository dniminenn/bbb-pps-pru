# Measurement

PTP cross-validation tooling. The full methodology, results, and reproduction steps
are in [CROSS-VALIDATION.md](../CROSS-VALIDATION.md) at the repository root.

- `cpts-phc2sys.service` disciplines the idle CPTS PHC (`/dev/ptp0`) from the
  GPS-driven system clock, so it mirrors this box's GPS time as a measurement reference.
- `ptp-agreement.sh` on-demand readout of the median time offset versus the grandmaster.
