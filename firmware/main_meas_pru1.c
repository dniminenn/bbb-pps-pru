/* main_meas_pru1.c
 * PRU1 measurement firmware: bracket CPSW stats reads against TSCTR.
 * Decides the poll quantum for PRU-assisted packet timestamping.
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

struct ev {
  uint32_t tsctr, rxg, txg;
};
struct meas {
  uint32_t magic; /* "PRU1" when live */
  uint32_t iters_lo, iters_hi;
  uint32_t lat_min, lat_max; /* CPSW read bracket, ticks */
  uint32_t base_lat;         /* TSCTR back-to-back, ticks */
  uint32_t hist[16];         /* 8-tick (40 ns) bins */
  uint32_t rx_events, tx_events;
  uint32_t seq;
  struct ev ring[64];
};
volatile struct meas *M = (volatile struct meas *)0x1000; /* ARM: 0x4A303000 */

void main(void) {
  uint32_t last_r, last_x, i, t1, t2, r, x, lat, b, s;
  for (i = 0; i < sizeof(struct meas) / 4; i++)
    ((volatile uint32_t *)M)[i] = 0;
  M->lat_min = 0xffffffff;
  t1 = TSCTR;
  t2 = TSCTR;
  M->base_lat = t2 - t1;
  last_r = RXGOOD;
  last_x = TXGOOD;
  M->magic = 0x50525531;
  for (;;) {
    t1 = TSCTR;
    r = RXGOOD;
    t2 = TSCTR;
    x = TXGOOD;
    lat = t2 - t1;
    if (lat < M->lat_min) M->lat_min = lat;
    if (lat > M->lat_max) M->lat_max = lat;
    b = lat >> 3;
    if (b > 15) b = 15;
    M->hist[b]++;
    if (!++M->iters_lo) M->iters_hi++;
    if (r != last_r || x != last_x) {
      s = M->seq;
      M->ring[s & 63].tsctr = t2;
      M->ring[s & 63].rxg = r;
      M->ring[s & 63].txg = x;
      M->seq = s + 1;
      M->rx_events += r - last_r;
      M->tx_events += x - last_x;
      last_r = r;
      last_x = x;
    }
  }
}
