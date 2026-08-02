/* main_ecap.c
 * PRU0 Firmware: ICSS eCAP hardware timestamping for PPS on P8_15
 *
 * The PPS edge is latched by the eCAP capture unit in silicon (CAP1, rising,
 * absolute mode, TSCTR free-running at the 200 MHz ICSS OCP clock). The PRU
 * only publishes the already-captured value, so nothing in this loop — not
 * poll granularity, not rpmsg servicing — can smear the timestamp.
 *
 * SPDX-License-Identifier: MIT-0
 * Copyright (c) 2026 dniminenn
 */

#include <stdint.h>
#include <string.h>
#include <pru_cfg.h>
#include <pru_intc.h>
#include <pru_types.h>
#include <pru_rpmsg.h>
#include <pru_virtqueue.h>
#include "intc_map_0.h"
#include "resource_table.h"

/* ICSS eCAP, PRU-local address 0x00030000 (ARM: 0x4A330000) */
#define ECAP_TSCTR (*(volatile uint32_t *)(0x00030000))
#define ECAP_CAP1 (*(volatile uint32_t *)(0x00030008))
#define ECAP_ECCTL1 (*(volatile uint16_t *)(0x00030028))
#define ECAP_ECCTL2 (*(volatile uint16_t *)(0x0003002A))
#define ECAP_ECFLG (*(volatile uint16_t *)(0x0003002E))
#define ECAP_ECCLR (*(volatile uint16_t *)(0x00030030))

volatile register uint32_t __R31;

#define HOST_INT ((uint32_t)1 << 30)
#define TO_ARM_HOST 16
#define FROM_ARM_HOST 17
#define CHAN_NAME "rpmsg-pru"
#define CHAN_PORT 30

struct pps_shared {
  volatile uint32_t seq;
  volatile uint32_t iep_lo; /* now an eCAP TSCTR capture; name kept for ABI */
};

#pragma DATA_SECTION(pps_data, ".pps_dram")
#pragma RETAIN(pps_data)
struct pps_shared pps_data = {0, 0};

/*
 * DS3231 TCXO 32.768 kHz on P9_41A (pr1_pru0_pru_r31_16, internal pull-up).
 * Falling edges (driven hard by the open-drain output) are counted and every
 * 4096 edges (125 ms) {edges, TSCTR} is snapshotted for the daemon: a live
 * measurement of the 200 MHz timebase against a ±2 ppm reference. The PPS
 * timestamp itself is hardware-latched in eCAP, so this bookkeeping costs
 * nothing but publication latency.
 */
#define TCXO_BIT (1u << 16)
#define EDGES_PER_SNAP 4096u

struct tcxo_shared {
  volatile uint32_t seq;
  volatile uint32_t edges;
  volatile uint32_t tsctr;
};

#pragma DATA_SECTION(tcxo_data, ".tcxo_dram")
#pragma RETAIN(tcxo_data)
struct tcxo_shared tcxo_data = {0, 0, 0};

static void ecap_init(void) {
  ECAP_ECCTL1 = 0x0100; /* CAPLDEN; CAP1 rising, absolute, no prescale */
  ECAP_ECCTL2 = 0x0010; /* TSCTRSTOP=run; continuous; wrap after CEVT1 */
  ECAP_TSCTR = 0;
  ECAP_ECCLR = 0xFFFF;
}

int main(void) {
  CT_CFG.SYSCFG_bit.STANDBY_INIT = 0;

  struct pru_rpmsg_transport transport;
  uint16_t src, dst, len;
  volatile uint8_t *status;
  char buf[32];

  /* Wait for rpmsg vdev to be ready */
  status = &resourceTable.rpmsg_vdev.status;
  while (!(*status & (1 << 2))) {
  }

  pru_rpmsg_init(&transport, &resourceTable.rpmsg_vring0,
                 &resourceTable.rpmsg_vring1, TO_ARM_HOST, FROM_ARM_HOST);

  while (pru_rpmsg_channel(RPMSG_NS_CREATE, &transport, CHAN_NAME, CHAN_PORT) !=
         PRU_RPMSG_SUCCESS) {
  }

  ecap_init();
  pps_data.seq = 0;
  pps_data.iep_lo = 0;

  /*
   * Wait for the daemon to send a setup byte — this tells us the
   * ARM-side endpoint addresses (src/dst) to use for notifications.
   */
  uint16_t arm_src = 0, arm_dst = 0;
  while (!arm_src) {
    if (__R31 & HOST_INT) {
      if (pru_rpmsg_receive(&transport, &arm_src, &arm_dst, buf, &len) ==
          PRU_RPMSG_SUCCESS) {
        /* got the endpoint pair — ready to notify */
      }
    }
  }

  char notify = 'P';
  uint32_t tcxo_prev = __R31 & TCXO_BIT;
  uint32_t tcxo_edges = 0, tcxo_since = 0;

  for (;;) {
    uint32_t tcxo_cur = __R31 & TCXO_BIT;
    if (!tcxo_cur && tcxo_prev) { /* TCXO falling edge */
      uint32_t now = ECAP_TSCTR;
      tcxo_edges++;
      if (++tcxo_since == EDGES_PER_SNAP) {
        tcxo_since = 0;
        tcxo_data.edges = tcxo_edges;
        tcxo_data.tsctr = now;
        tcxo_data.seq++;
      }
    }
    tcxo_prev = tcxo_cur;

    if (ECAP_ECFLG & 0x0002) { /* CEVT1: an edge was captured in hardware */
      pps_data.iep_lo = ECAP_CAP1;
      pps_data.seq++;
      ECAP_ECCLR = 0x0003; /* clear CEVT1 + INT */
      pru_rpmsg_send(&transport, arm_dst, arm_src, &notify, 1);

      /* Service host kicks here, in the pulse's shadow; the timestamp is
       * hardware-latched so this only affects notification latency. */
      if (__R31 & HOST_INT) {
        CT_INTC.SICR_bit.STS_CLR_IDX = FROM_ARM_HOST;
        while (pru_rpmsg_receive(&transport, &arm_src, &arm_dst, buf, &len) ==
               PRU_RPMSG_SUCCESS) {
        }
      }
    }
  }
}
