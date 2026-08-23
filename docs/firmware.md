# PRU Firmware

### Project Layout

The firmware component lives in the [`firmware/`](../firmware/) directory:
- [`main.c`](../firmware/main.c): PRU0 firmware
- [`resource_table.h`](../firmware/resource_table.h): rpmsg vdev resource table
- [`intc_map_0.h`](../firmware/intc_map_0.h): INTC interrupt map
- [`AM335x_PRU_intc_rscTbl.cmd`](../firmware/AM335x_PRU_intc_rscTbl.cmd): linker command file

### [main.c](../firmware/main.c): PRU0 Firmware

The PRU firmware:
1. Initializes rpmsg (required for remoteproc to consider the PRU "ready")
2. Starts the IEP counter in free-running mode at 200 MHz
3. Waits for the daemon to send a setup byte via `/dev/rpmsg_pru30` (learns the ARM endpoint addresses)
4. Polls `R31` bit 14 for a rising edge on P8_16
5. On each rising edge, latches `IEP_COUNT_LO`, increments a sequence number, and sends an rpmsg notification to wake the daemon
6. Stores `{seq, iep_lo}` in PRU DRAM0 at offset 0x0 (physical 0x4A300000)

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

The `.pps_dram` section at offset 0 of PAGE 1 (PRU DRAM0) means the `pps_shared` struct is always at physical address `0x4A300000`, which is hardcoded in the userspace daemon.

### Building the Firmware

```bash
cd /opt/source/pru-pps/firmware

export PRU_CGT=/usr/share/ti/cgt-pru
PSSP=/opt/source/pssp
rm -f gen/*

# Compile
$PRU_CGT/bin/clpru --silicon_version=3 -O2 \
  --include_path=$PRU_CGT/include \
  --include_path=$PSSP/include \
  --include_path=$PSSP/include/am335x \
  --display_error_number --endian=little --hardware_mac=on \
  --obj_directory=gen --pp_directory=gen -ppd -ppa \
  -fe gen/main.obj main.c

# Link
$PRU_CGT/bin/clpru --silicon_version=3 -O2 \
  --display_error_number --endian=little --hardware_mac=on \
  -z -i$PRU_CGT/lib -i$PRU_CGT/include \
  --reread_libs --warn_sections --stack_size=0x100 --heap_size=0x100 \
  -o gen/pru-pps.out gen/main.obj \
  -m gen/pru-pps.map \
  ./AM335x_PRU_intc_rscTbl.cmd \
  --library=libc.a \
  --library=$PSSP/lib/rpmsg_lib.lib

# Verify sections
readelf -S gen/pru-pps.out | grep -E 'pps_dram|resource|irq'

# Install firmware
cp gen/pru-pps.out /lib/firmware/am335x-pru0-fw
```

The remoteproc driver loads `/lib/firmware/am335x-pru0-fw` by default for PRU0.

[Next: Userspace Daemon & Chrony](daemon.md) | [Previous: Initial Setup & Overlays](setup.md)

### main_meas_pru1.c: PRU1 CPSW poll-latency measurement

Build like main.c but link with the `.pru_irq_map` output section removed from a
copy of the linker cmd (the empty COPY section fails remoteproc's loader) and no
rpmsg lib. Measured on chron: 225 ns typical CPSW stats read from the PRU, 604 ns
full rx+tx poll loop, 20 ns local TSCTR read. Groundwork for PRU-assisted packet
timestamping (poll RXGOODFRAMES/TXGOODFRAMES, latch shared TSCTR per tick).
