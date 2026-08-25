# PRU Firmware

### Project Layout

The firmware component lives in the [`firmware/`](../firmware/) directory:
- [`main_ecap.c`](../firmware/main_ecap.c): **PRU0 production firmware**: eCAP PPS capture + DS3231 32 kHz edge counting
- [`main_pktts_pru1.c`](../firmware/main_pktts_pru1.c): **PRU1 production firmware**: per-frame packet timestamping ring for the cpsw MAC
- [`main_meas_pru1.c`](../firmware/main_meas_pru1.c): PRU1 poll-latency measurement tool
- [`resource_table.h`](../firmware/resource_table.h): rpmsg vdev resource table
- [`intc_map_0.h`](../firmware/intc_map_0.h): INTC interrupt map
- [`AM335x_PRU_intc_rscTbl.cmd`](../firmware/AM335x_PRU_intc_rscTbl.cmd): linker command file

### PRU0: [main_ecap.c](../firmware/main_ecap.c)

1. Initializes rpmsg (required for remoteproc to consider the PRU "ready")
2. Programs the ICSS eCAP: CAP1 latches the rising PPS edge in silicon against the free-running 200 MHz TSCTR (`ECCTL1 = CAPLDEN, absolute, no prescale`; `ECCTL2 = run, continuous`)
3. On each CEVT1 flag, reads the latched CAP1 value, increments a sequence number, and notifies the daemon over rpmsg; nothing software-timed touches the edge
4. Counts the DS3231 32.768 kHz falling edges on R31 bit 16 and snapshots `{edges, TSCTR}` every 4096 edges into shared DRAM, giving the daemon a continuous crystal-vs-TCXO frequency measurement
5. Stores `{seq, capture}` in PRU DRAM0 at offset 0x0 (physical 0x4A300000)

The same TSCTR register is what the `ptp_pruss` kernel module extends to 64 bits and
exposes as the PHC, so PPS captures, packet stamps, and the PHC all live on one
timebase.

### PRU1: [main_pktts_pru1.c](../firmware/main_pktts_pru1.c)

Polls the cpsw MAC statistics counters (RXGOODFRAMES/TXGOODFRAMES) in a tight loop
and latches the shared TSCTR whenever a counter moves, writing `{count, ticks}`
entries into a ring in shared memory. The `cpsw_pruts` kernel module matches ring
entries to skbs by arrival order and installs them as ordinary kernel hardware
timestamps. Frame-count deltas make the matching robust; ambiguous bursts are
counted and surfaced rather than guessed. Poll costs (measurable with
`main_meas_pru1.c`): 225 ns per CPSW stats read, 604 ns full rx+tx loop, 20 ns
local TSCTR read.

### [resource_table.h](../firmware/resource_table.h)

Declares one rpmsg vdev with two vrings (VQ size 16 each). Required for the remoteproc driver to set up the rpmsg channel and mark the PRU as running.

Key defines:
- `CHAN_NAME "rpmsg-pru"`: matches what the kernel rpmsg_pru driver expects
- `CHAN_PORT 30`: creates `/dev/rpmsg_pru30`

### [intc_map_0.h](../firmware/intc_map_0.h)

Maps sysevt 17 → channel 0 → host interrupt 0. This is the kick interrupt from the ARM host to PRU0.

### [AM335x_PRU_intc_rscTbl.cmd](../firmware/AM335x_PRU_intc_rscTbl.cmd): Linker Command File

Key section placement:

```
.pps_dram  > 0x0, PAGE 1    /* PRU DRAM0 offset 0 = physical 0x4A300000 */
.resource_table > PRU_DMEM_0_1, PAGE 1
.text      > PRU_IMEM, PAGE 0
```

The `.pps_dram` section at offset 0 of PAGE 1 (PRU DRAM0) means the shared struct is always at physical address `0x4A300000`, which is hardcoded in the userspace daemon.

### Building

Both firmwares build with the same TI toolchain invocation; substitute the source
file and output name:

```bash
cd firmware
export PRU_CGT=/usr/share/ti/cgt-pru
PSSP=/opt/source/pssp
rm -f gen/*

# Compile (main_ecap.c shown; same for main_pktts_pru1.c)
$PRU_CGT/bin/clpru --silicon_version=3 -O2 \
  --include_path=$PRU_CGT/include \
  --include_path=$PSSP/include \
  --include_path=$PSSP/include/am335x \
  --display_error_number --endian=little --hardware_mac=on \
  --obj_directory=gen --pp_directory=gen -ppd -ppa \
  -fe gen/main_ecap.obj main_ecap.c

# Link
$PRU_CGT/bin/clpru --silicon_version=3 -O2 \
  --display_error_number --endian=little --hardware_mac=on \
  -z -i$PRU_CGT/lib -i$PRU_CGT/include \
  --reread_libs --warn_sections --stack_size=0x100 --heap_size=0x100 \
  -o gen/pru-pps.out gen/main_ecap.obj \
  -m gen/pru-pps.map \
  ./AM335x_PRU_intc_rscTbl.cmd \
  --library=libc.a \
  --library=$PSSP/lib/rpmsg_lib.lib

# Install
cp gen/pru-pps.out /lib/firmware/am335x-pru0-fw           # PRU0, remoteproc default
cp gen/pru-pktts.out /lib/firmware/am335x-pru1-fw-pktts   # PRU1, selected by pruts.service
```

remoteproc loads `/lib/firmware/am335x-pru0-fw` for PRU0 by default;
[`kernel/pruts.service`](../kernel/pruts.service) writes the pktts firmware name
into remoteproc2 before booting PRU1 and inserting the modules. PRU1 firmwares
without rpmsg link with the `.pru_irq_map` COPY section removed from a copy of the
linker cmd (the empty section fails remoteproc's loader) and no rpmsg lib.

[Next: Userspace Daemon & Chrony](daemon.md) | [Previous: Initial Setup & Overlays](setup.md)
