/* main_pktts_pru1.c
 * PRU1 packet-timestamp firmware: poll CPSW RXGOODFRAMES/TXGOODFRAMES,
 * latch the shared eCAP TSCTR on every change, publish a ring.
 * ~600 ns poll quantum (225 ns per CPSW read, measured).
 */
#include <stdint.h>

/* bare resource table, no vrings */
struct rsc_table {
  uint32_t ver, num, reserved[2];
};
#pragma DATA_SECTION(pru_remoteproc_ResourceTable, ".resource_table")
#pragma RETAIN(pru_remoteproc_ResourceTable)
struct rsc_table pru_remoteproc_ResourceTable = {1, 0, {0, 0}};

#define TSCTR (*(volatile uint32_t *)(0x00030000)) /* shared ICSS eCAP */
#define CPSW_STATS 0x4A100900
#define RXGOOD (*(volatile uint32_t *)(CPSW_STATS + 0x00))
#define TXGOOD (*(volatile uint32_t *)(CPSW_STATS + 0x34))

#define RING 128

struct ev {
  uint32_t tsctr; /* after the detecting read */
  uint32_t rxg, txg;
};
struct pktts {
  uint32_t magic; /* "PKTS" when live */
  uint32_t seq;   /* total events written */
  uint32_t iters_lo, iters_hi;
  uint32_t rx0, tx0; /* counter values at start */
  struct ev ring[RING];
};
volatile struct pktts *P = (volatile struct pktts *)0x1000; /* ARM: 0x4A303000 */

void main(void) {
  uint32_t last_r, last_x, i, r, x, t, s;
  for (i = 0; i < sizeof(struct pktts) / 4; i++)
    ((volatile uint32_t *)P)[i] = 0;
  last_r = RXGOOD;
  last_x = TXGOOD;
  P->rx0 = last_r;
  P->tx0 = last_x;
  P->magic = 0x504b5453;
  for (;;) {
    r = RXGOOD;
    x = TXGOOD;
    t = TSCTR;
    if (!++P->iters_lo) P->iters_hi++;
    if (r != last_r || x != last_x) {
      s = P->seq;
      P->ring[s & (RING - 1)].tsctr = t;
      P->ring[s & (RING - 1)].rxg = r;
      P->ring[s & (RING - 1)].txg = x;
      P->seq = s + 1; /* entry valid once seq moves */
      last_r = r;
      last_x = x;
    }
  }
}
