/* main.c
 * PRU0 Firmware: IEP timestamping for PPS signal on P8_16
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

#define IEP_GLOBAL_CFG (*(volatile uint32_t *)(0x0002E000))
#define IEP_COMPEN (*(volatile uint32_t *)(0x0002E008))
#define IEP_COUNT_LO (*(volatile uint32_t *)(0x0002E00C))

volatile register uint32_t __R31;

#define HOST_INT ((uint32_t)1 << 30)
#define TO_ARM_HOST 16
#define FROM_ARM_HOST 17
#define CHAN_NAME "rpmsg-pru"
#define CHAN_PORT 30
#define PPS_BIT (1u << 14) /* P8_16 = pr1_pru0_pru_r31_14 */

struct pps_shared {
  volatile uint32_t seq;
  volatile uint32_t iep_lo;
};

#pragma DATA_SECTION(pps_data, ".pps_dram")
#pragma RETAIN(pps_data)
struct pps_shared pps_data = {0, 0};

static void iep_init(void) {
  IEP_GLOBAL_CFG = 0;
  IEP_COUNT_LO = 0;
  IEP_COMPEN = 0;
  IEP_GLOBAL_CFG = 0x11; /* enable, increment by 1 each cycle */
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

  iep_init();
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

  uint32_t prev = __R31 & PPS_BIT;
  char notify = 'P';

  for (;;) {
    uint32_t cur = __R31 & PPS_BIT;

    if (cur && !prev) {
      pps_data.iep_lo = IEP_COUNT_LO;
      pps_data.seq++;
      /* Wake the daemon via rpmsg */
      pru_rpmsg_send(&transport, arm_dst, arm_src, &notify, 1);

      /*
       * Service host kicks ONLY here, in the pulse's shadow — the next
       * rising edge is a full second away. Draining in the tight loop cost
       * a vring scan between every R31 poll (the kick sysevent was never
       * cleared, so HOST_INT stayed asserted after the first kick), which
       * smeared edge detection by hundreds of ns. Clearing SICR re-arms
       * the event; anything the host sends mid-second is handled after the
       * next pulse, which the setup-byte protocol tolerates.
       */
      if (__R31 & HOST_INT) {
        CT_INTC.SICR_bit.STS_CLR_IDX = FROM_ARM_HOST;
        while (pru_rpmsg_receive(&transport, &arm_src, &arm_dst, buf, &len) ==
               PRU_RPMSG_SUCCESS) {
        }
      }
    }
    prev = cur;
  }
}
