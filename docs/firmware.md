# PRU Firmware

### Project Layout

The firmware component lives in the [`firmware/`](../firmware/) directory:
- [`main.c`](../firmware/main.c) — PRU0 firmware
- [`resource_table.h`](../firmware/resource_table.h) — rpmsg vdev resource table
- [`intc_map_0.h`](../firmware/intc_map_0.h) — INTC interrupt map
- [`AM335x_PRU_intc_rscTbl.cmd`](../firmware/AM335x_PRU_intc_rscTbl.cmd) — linker command file

### [main.c](../firmware/main.c) — PRU0 Firmware

The PRU firmware:
1. Initializes rpmsg (required for remoteproc to consider the PRU "ready")
2. Starts the IEP counter in free-running mode at 200 MHz
3. Polls `R31` bit 14 for a rising edge on P8_16
4. On each rising edge, latches `IEP_COUNT_LO` and increments a sequence number
5. Stores `{seq, iep_lo}` in PRU DRAM0 at offset 0x0 (physical 0x4A300000)

### [resource_table.h](../firmware/resource_table.h)

Declares one rpmsg vdev with two vrings (VQ size 16 each). Required for the remoteproc driver to set up the rpmsg channel and mark the PRU as running.

Key defines:
- `CHAN_NAME "rpmsg-pru"` — matches what the kernel rpmsg_pru driver expects
- `CHAN_PORT 30` — creates `/dev/rpmsg_pru30`

### [intc_map_0.h](../firmware/intc_map_0.h)

Maps sysevt 17 → channel 0 → host interrupt 0. This is the kick interrupt from the ARM host to PRU0.

### [AM335x_PRU_intc_rscTbl.cmd](../firmware/AM335x_PRU_intc_rscTbl.cmd) — Linker Command File

Key section placement:

```
.pps_dram  > 0x0, PAGE 1    /* PRU DRAM0 offset 0 = physical 0x4A300000 */
.resource_table > PRU_DMEM_0_1, PAGE 1
.text      > PRU_IMEM, PAGE 0
```

The `.pps_dram` section at offset 0 of PAGE 1 (PRU DRAM0) means the `pps_shared` struct is always at physical address `0x4A300000` — hardcoded in the userspace daemon.

### Building the Firmware

```bash
cd /opt/source/pru-pps/firmware

# Compile
clpru -v3 -O2 --include_path=/usr/lib/ti/pru-software-support-package/include \
      --include_path=/usr/lib/ti/pru-software-support-package/include/am335x \
      -DCORE0 main.c -o gen/main.obj

# Link
clpru -v3 -z AM335x_PRU_intc_rscTbl.cmd gen/main.obj \
      --library=/usr/lib/ti/pru-software-support-package/lib/rpmsg_lib.lib \
      -o gen/pru-pps.out \
      --map_file=gen/pru-pps.map

# Install firmware
cp gen/pru-pps.out /lib/firmware/am335x-pru0-fw
```

The remoteproc driver loads `/lib/firmware/am335x-pru0-fw` by default for PRU0.

[Next: Userspace Daemon & Chrony](daemon.md) | [Previous: Initial Setup & Overlays](setup.md)
