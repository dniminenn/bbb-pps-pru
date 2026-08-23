/* pktts_ntpd.c
 * NTP server with PRU-assisted hardware packet timestamps.
 * t2/t3 from CPSW frame-counter ticks latched by PRU1 (main_pktts_pru1.c),
 * converted via a TSCTR<->CLOCK_REALTIME fit. chrony keeps disciplining
 * the clock (run it with "port 0"); this daemon only serves.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>

#define PRU1_DRAM 0x4A302000
#define PKTTS_OFF 0x1000
#define RING 128
#define TSCTR_HZ 200e6

struct ev { uint32_t tsctr, rxg, txg; };
struct pktts {
  uint32_t magic, seq, iters_lo, iters_hi, rx0, tx0;
  struct ev ring[RING];
};

/* ---- tick unwrap + fit ---- */
#define FITN 96
struct fitpt { int64_t tick; long long rt; };
static struct fitpt fitring[FITN];
static int fit_n = 0, fit_head = 0;
static double fit_slope = 5.0;     /* ns per tick */
static double fit_off = 0;         /* relative intercept */
static int64_t fit_base = 0;
static long long fit_rbase = 0;    /* absolute anchor */
static int fit_valid = 0;

static volatile uint32_t *tsctr_reg;
static int64_t tick_ext = 0;
static uint32_t tick_last = 0;

static int64_t tick_now(void) {
  uint32_t raw = *tsctr_reg;
  tick_ext += (int32_t)(raw - tick_last);
  tick_last = raw;
  return tick_ext;
}
/* place a raw ring tick near the unwrapped timeline */
static int64_t tick_place(uint32_t raw) {
  return tick_ext + (int32_t)(raw - tick_last);
}

static void fit_push(int64_t tick, long long rt) {
  fitring[fit_head].tick = tick;
  fitring[fit_head].rt = rt;
  fit_head = (fit_head + 1) % FITN;
  if (fit_n < FITN) fit_n++;
}

static void fit_solve(void) {
  if (fit_n < 16) { fit_valid = 0; return; }
  int i, idx = (fit_head + FITN - fit_n) % FITN;
  int64_t bt = fitring[idx].tick;
  long long br = fitring[idx].rt;
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (i = 0; i < fit_n; i++) {
    struct fitpt *s = &fitring[(idx + i) % FITN];
    double x = (double)(s->tick - bt), y = (double)(s->rt - br);
    sx += x; sy += y; sxx += x * x; sxy += x * y;
  }
  double n = fit_n, det = n * sxx - sx * sx;
  if (det <= 0) { fit_valid = 0; return; }
  fit_slope = (n * sxy - sx * sy) / det;
  fit_off = (sy - fit_slope * sx) / n;
  fit_base = bt;
  /* chrony steps flush the window */
  if (fit_slope < 4.9 || fit_slope > 5.1) { fit_n = 0; fit_valid = 0; return; }
  fit_rbase = br;
  fit_valid = 1;
}

static long long tick_to_wall(int64_t tick) {
  return fit_rbase +
         (long long)(fit_off + fit_slope * (double)(tick - fit_base));
}

static void sample_fit(void) {
  struct timespec a, b;
  long long best = LLONG_MAX; int64_t bt = 0; long long brt = 0;
  for (int k = 0; k < 3; k++) {
    clock_gettime(CLOCK_REALTIME, &a);
    int64_t t = tick_now();
    clock_gettime(CLOCK_REALTIME, &b);
    long long an = (long long)a.tv_sec * 1000000000LL + a.tv_nsec;
    long long bn = (long long)b.tv_sec * 1000000000LL + b.tv_nsec;
    if (bn - an < best) { best = bn - an; bt = t; brt = an + (bn - an) / 2; }
  }
  if (best < 6000) fit_push(bt, brt);
  fit_solve();
}

/* ---- chrony state mirror ---- */
static int g_stratum = 16;
static int g_leap = 3;
static double g_rootdelay = 0, g_rootdisp = 1;
static void poll_chrony(void) {
  FILE *p = popen("chronyc -c tracking 2>/dev/null", "r");
  if (!p) return;
  char line[512];
  if (fgets(line, sizeof(line), p)) {
    /* csv: refid,name,stratum,...,rootdelay(10),rootdisp(11) */
    char *f[16]; int nf = 0;
    for (char *t = strtok(line, ","); t && nf < 16; t = strtok(NULL, ","))
      f[nf++] = t;
    if (nf >= 12) {
      g_stratum = atoi(f[2]);
      g_rootdelay = atof(f[10]);
      g_rootdisp = atof(f[11]);
      g_leap = (g_stratum >= 16) ? 3 : 0;
    }
  }
  pclose(p);
}

/* ---- ring consumption ---- */
static volatile struct pktts *pk;
static uint32_t ring_seen = 0;
struct wallev { long long wall; int rx; };  /* rx=1 rx event */
#define WEN 64
static struct wallev wev[WEN];
static int wev_n = 0;

static void drain_ring(void) {
  uint32_t s = pk->seq;
  if (s == ring_seen) return;
  uint32_t start = (s - ring_seen > RING) ? s - RING : ring_seen;
  static uint32_t prx = 0, ptx = 0;
  for (uint32_t i = start; i != s; i++) {
    struct ev e;
    e.tsctr = pk->ring[i & (RING - 1)].tsctr;
    e.rxg = pk->ring[i & (RING - 1)].rxg;
    e.txg = pk->ring[i & (RING - 1)].txg;
    if (!fit_valid) { prx = e.rxg; ptx = e.txg; continue; }
    long long w = tick_to_wall(tick_place(e.tsctr));
    if (e.rxg != prx) {
      memmove(&wev[0], &wev[1], sizeof(wev[0]) * (WEN - 1));
      wev[WEN - 1].wall = w; wev[WEN - 1].rx = 1;
      if (wev_n < WEN) wev_n++;
    }
    if (e.txg != ptx) {
      memmove(&wev[0], &wev[1], sizeof(wev[0]) * (WEN - 1));
      wev[WEN - 1].wall = w; wev[WEN - 1].rx = 0;
      if (wev_n < WEN) wev_n++;
    }
    prx = e.rxg; ptx = e.txg;
  }
  ring_seen = s;
}

/* learned kernel stamp latency */
static double klat_ns = 60000;
static unsigned g_match = 0, g_fallback = 0, g_ambig = 0, g_served = 0;

static long long match_rx(long long kstamp) {
  long long target = kstamp - (long long)klat_ns;
  long long best = 0, bestd = LLONG_MAX, second = LLONG_MAX;
  for (int i = 0; i < WEN; i++) {
    if (!wev[i].rx || !wev[i].wall) continue;
    long long d = target - wev[i].wall;
    long long ad = d < 0 ? -d : d;
    if (ad < bestd) { second = bestd; bestd = ad; best = wev[i].wall; }
    else if (ad < second) second = ad;
  }
  if (!best || bestd > 200000) { g_fallback++; return 0; }
  if (second < 30000) { g_ambig++; return 0; }   /* two frames too close */
  klat_ns += 0.02 * ((double)(kstamp - best) - klat_ns);
  g_match++;
  return best;
}

static long long match_tx_after(long long after) {
  for (int i = WEN - 1; i >= 0; i--) {
    if (wev[i].rx || !wev[i].wall) continue;
    if (wev[i].wall > after && wev[i].wall - after < 3000000)
      return wev[i].wall;
  }
  return 0;
}

/* ---- NTP ---- */
#define NTP_EPOCH_DELTA 2208988800ULL
static void wall_to_ntp(long long ns, uint32_t *sec, uint32_t *frac) {
  long long s = ns / 1000000000LL, r = ns % 1000000000LL;
  *sec = htonl((uint32_t)(s + NTP_EPOCH_DELTA));
  *frac = htonl((uint32_t)((double)r * 4.294967296));
}

struct peer {
  uint32_t ip; uint16_t port;
  uint8_t rx_ts[8];       /* last t2 we told them */
  uint8_t tx_ts[8];       /* measured tx of last reply */
  int valid;
};
static struct peer peers[256];

static double egress_ns = 100000;  /* send to wire, EWMA */
static long long cal_ns = 0;       /* serve calibration */

static void serve(int fd) {
  uint8_t buf[128], ctrl[256];
  struct sockaddr_in from;
  struct iovec iov = {buf, sizeof(buf)};
  struct msghdr mh = {&from, sizeof(from), &iov, 1, ctrl, sizeof(ctrl), 0};
  ssize_t n = recvmsg(fd, &mh, 0);
  if (n < 48) return;
  if ((buf[0] & 7) != 3) return;   /* client mode only */
  long long kstamp = 0;
  for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_TIMESTAMPING) {
      struct timespec *ts = (struct timespec *)CMSG_DATA(c);
      kstamp = (long long)ts[0].tv_sec * 1000000000LL + ts[0].tv_nsec;
    }
  if (!kstamp) {
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    kstamp = (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
  }
  drain_ring();
  long long t2 = match_rx(kstamp);
  if (!t2) t2 = kstamp - (long long)klat_ns;   /* fallback */
  t2 += cal_ns;

  struct peer *pe = &peers[(ntohl(from.sin_addr.s_addr) ^ ntohs(from.sin_port)) & 255];
  int xleave = pe->valid && !memcmp(&buf[24], pe->rx_ts, 8) &&
               pe->ip == from.sin_addr.s_addr && pe->port == from.sin_port;

  uint8_t rsp[48];
  memset(rsp, 0, sizeof(rsp));
  rsp[0] = (uint8_t)((g_leap << 6) | (4 << 3) | 4);
  rsp[1] = (uint8_t)g_stratum;
  rsp[2] = buf[2]; rsp[3] = (uint8_t)-29;
  uint32_t rd = (uint32_t)(g_rootdelay * 65536.0);
  uint32_t rdsp = (uint32_t)((g_rootdisp + 2e-7) * 65536.0);
  memcpy(&rsp[4], &(uint32_t){htonl(rd)}, 4);
  memcpy(&rsp[8], &(uint32_t){htonl(rdsp)}, 4);
  memcpy(&rsp[12], "PPS\0", 4);
  /* reference time: now, coarse */
  {
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    uint32_t s, f;
    wall_to_ntp((long long)now.tv_sec * 1000000000LL, &s, &f);
    memcpy(&rsp[16], &s, 4); memcpy(&rsp[20], &f, 4);
  }
  /* origin: xleave = client rx ts (buf 32); basic = client tx ts (buf 40) */
  memcpy(&rsp[24], xleave ? &buf[32] : &buf[40], 8);
  uint32_t t2s, t2f;
  wall_to_ntp(t2, &t2s, &t2f);
  memcpy(&rsp[32], &t2s, 4); memcpy(&rsp[36], &t2f, 4);

  long long t3;
  if (xleave && pe->tx_ts[0] | pe->tx_ts[1]) {
    memcpy(&rsp[40], pe->tx_ts, 8);
    t3 = 0;
  } else {
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    t3 = (long long)now.tv_sec * 1000000000LL + now.tv_nsec
       + (long long)egress_ns + cal_ns;
    uint32_t s, f; wall_to_ntp(t3, &s, &f);
    memcpy(&rsp[40], &s, 4); memcpy(&rsp[44], &f, 4);
    if (!memcmp(&rsp[40], &rsp[32], 8)) rsp[47] ^= 1;  /* rfc9769 */
  }
  long long presend;
  { struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    presend = (long long)now.tv_sec * 1000000000LL + now.tv_nsec; }
  if (sendto(fd, rsp, 48, 0, (struct sockaddr *)&from, sizeof(from)) != 48)
    return;
  g_served++;
  /* measured egress from the tx event */
  usleep(200);
  drain_ring();
  long long txw = match_tx_after(presend - 50000);
  if (txw) {
    egress_ns += 0.05 * ((double)(txw - presend) - egress_ns);
    uint32_t s, f; wall_to_ntp(txw + cal_ns, &s, &f);
    memcpy(pe->tx_ts, &s, 4); memcpy(pe->tx_ts + 4, &f, 4);
  } else {
    memset(pe->tx_ts, 0, 8);
  }
  memcpy(pe->rx_ts, &rsp[32], 8);
  pe->ip = from.sin_addr.s_addr;
  pe->port = from.sin_port;
  pe->valid = 1;
}

static void write_prom(void) {
  FILE *f = fopen("/run/pruts/pktts.prom.tmp", "w");
  if (!f) return;
  fprintf(f,
    "# HELP pktts_served_total NTP replies served\n"
    "# TYPE pktts_served_total counter\npktts_served_total %u\n"
    "# HELP pktts_match_total t2 from PRU ring\n"
    "# TYPE pktts_match_total counter\npktts_match_total %u\n"
    "# HELP pktts_fallback_total t2 from kernel stamp\n"
    "# TYPE pktts_fallback_total counter\npktts_fallback_total %u\n"
    "# HELP pktts_ambiguous_total frames too close to match\n"
    "# TYPE pktts_ambiguous_total counter\npktts_ambiguous_total %u\n"
    "# HELP pktts_kernel_latency_ns learned frame-end to kernel stamp\n"
    "# TYPE pktts_kernel_latency_ns gauge\npktts_kernel_latency_ns %.0f\n"
    "# HELP pktts_egress_ns learned send to wire\n"
    "# TYPE pktts_egress_ns gauge\npktts_egress_ns %.0f\n"
    "# HELP pktts_fit_valid tick-to-wall fit solved\n"
    "# TYPE pktts_fit_valid gauge\npktts_fit_valid %d\n",
    g_served, g_match, g_fallback, g_ambig, klat_ns, egress_ns, fit_valid);
  fclose(f);
  rename("/run/pruts/pktts.prom.tmp", "/run/pruts/pktts.prom");
}

int main(int argc, char **argv) {
  int opt, port = 123;
  while ((opt = getopt(argc, argv, "c:p:")) != -1) {
    if (opt == 'c') cal_ns = atoll(optarg);
    if (opt == 'p') port = atoi(optarg);
  }
  int memfd = open("/dev/mem", O_RDONLY | O_SYNC);
  if (memfd < 0) { perror("/dev/mem"); return 1; }
  volatile uint8_t *dram = mmap(NULL, 0x2000, PROT_READ, MAP_SHARED, memfd,
                                PRU1_DRAM);
  volatile uint8_t *ecap = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, memfd,
                                0x4A330000);
  if (dram == MAP_FAILED || ecap == MAP_FAILED) { perror("mmap"); return 1; }
  pk = (volatile struct pktts *)(dram + PKTTS_OFF);
  tsctr_reg = (volatile uint32_t *)ecap;
  if (pk->magic != 0x504b5453) {
    fprintf(stderr, "pktts_ntpd: PRU1 firmware not live\n");
    return 1;
  }
  tick_last = *tsctr_reg;

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  int tsflags = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
  setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &tsflags, sizeof(tsflags));
  struct sockaddr_in sa = {AF_INET, htons(port), {INADDR_ANY}};
  if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    perror("bind"); return 1;
  }
  struct sched_param sp = {.sched_priority = 40};
  sched_setscheduler(0, SCHED_FIFO, &sp);
  mlockall(MCL_CURRENT | MCL_FUTURE);

  ring_seen = pk->seq;
  poll_chrony();
  long long last_chrony = 0, last_prom = 0;
  printf("pktts_ntpd: serving :%d (cal %lld ns)\n", port, cal_ns);
  for (;;) {
    struct pollfd p = {fd, POLLIN, 0};
    int r = poll(&p, 1, 25);
    struct timespec mono; clock_gettime(CLOCK_MONOTONIC, &mono);
    long long now = (long long)mono.tv_sec * 1000000000LL + mono.tv_nsec;
    sample_fit();
    drain_ring();
    if (r > 0 && (p.revents & POLLIN)) serve(fd);
    if (now - last_chrony > 5000000000LL) { poll_chrony(); last_chrony = now; }
    if (now - last_prom > 10000000000LL) { write_prom(); last_prom = now; }
  }
}
