#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>

/*
 * NTP SHM struct matching gpsd/chrony on this system.
 * time_t is 8 bytes (64-bit time on 32-bit ARM, Debian trixie Y2038).
 *
 * offset  0: int mode (4)
 * offset  4: int count (4)
 * offset  8: time_t clockTimeStampSec (8)
 * offset 16: int clockTimeStampUSec (4)
 * offset 20: pad (4)
 * offset 24: time_t receiveTimeStampSec (8)
 * offset 32: int receiveTimeStampUSec (4)
 * offset 36: int leap (4)
 * offset 40: int precision (4)
 * offset 44: int nsamples (4)
 * offset 48: int valid (4)
 * offset 52: pad (4)
 * offset 56: unsigned clockTimeStampNSec (4)
 * offset 60: unsigned receiveTimeStampNSec (4)
 * offset 64: int dummy[8] (32)
 * total: 96 bytes
 */
struct shmTime {
  int32_t mode;                  /*  0 */
  int32_t count;                 /*  4 */
  int64_t clockTimeStampSec;     /*  8 */
  int32_t clockTimeStampUSec;    /* 16 */
  int32_t _pad1;                 /* 20 */
  int64_t receiveTimeStampSec;   /* 24 */
  int32_t receiveTimeStampUSec;  /* 32 */
  int32_t leap;                  /* 36 */
  int32_t precision;             /* 40 */
  int32_t nsamples;              /* 44 */
  int32_t valid;                 /* 48 */
  int32_t _pad2;                 /* 52 */
  uint32_t clockTimeStampNSec;   /* 56 */
  uint32_t receiveTimeStampNSec; /* 60 */
  int32_t dummy[8];              /* 64 */
};

struct pru_pps_data {
  volatile uint32_t seq;
  volatile uint32_t iep_lo;
};

static volatile int running = 1;
static void sighandler(int sig) {
  (void)sig;
  running = 0;
}

static struct shmTime *shm_get(int unit) {
  int id = shmget(0x4e545030 + unit, sizeof(struct shmTime), IPC_CREAT | 0600);
  if (id < 0) {
    perror("shmget");
    return NULL;
  }
  struct shmTime *s = (struct shmTime *)shmat(id, NULL, 0);
  if (s == (void *)-1) {
    perror("shmat");
    return NULL;
  }
  return s;
}

#define PRU_DRAM0_BASE 0x4A300000
#define IEP_BASE 0x4A32E000

int main(int argc, char **argv) {
  int shmunit = 2;
  int opt;

  while ((opt = getopt(argc, argv, "s:")) != -1) {
    switch (opt) {
    case 's':
      shmunit = atoi(optarg);
      break;
    default:
      fprintf(stderr, "Usage: %s [-s shmunit]\n", argv[0]);
      return 1;
    }
  }

  signal(SIGINT, sighandler);
  signal(SIGTERM, sighandler);

  int memfd = open("/dev/mem", O_RDONLY | O_SYNC);
  if (memfd < 0) {
    perror("/dev/mem");
    return 1;
  }

  volatile struct pru_pps_data *pru = (volatile struct pru_pps_data *)mmap(
      NULL, 0x1000, PROT_READ, MAP_SHARED, memfd, PRU_DRAM0_BASE);
  if (pru == MAP_FAILED) {
    perror("mmap DRAM0");
    return 1;
  }

  volatile uint32_t *iep = (volatile uint32_t *)mmap(
      NULL, 0x1000, PROT_READ, MAP_SHARED, memfd, IEP_BASE);
  if (iep == MAP_FAILED) {
    perror("mmap IEP");
    return 1;
  }
  close(memfd);

  struct shmTime *shm = shm_get(shmunit);
  if (!shm)
    return 1;

  memset(shm, 0, sizeof(struct shmTime));
  shm->mode = 1;
  shm->precision = -29; /* ~2ns */
  shm->nsamples = 3;
  shm->leap = 0;
  shm->valid = 0;

  struct sched_param sp = {.sched_priority = 50};
  if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
    perror("sched_setscheduler (non-fatal)");
  mlockall(MCL_CURRENT | MCL_FUTURE);

  uint32_t last_seq = pru->seq;
  uint32_t prev_pps_iep = 0;
  int have_prev = 0;
  double ns_per_tick = 4.99971;
  uint32_t good = 0, bad = 0;
  uint32_t saved_delta = 0;

  printf("pru_pps_shm: polling PRU DRAM -> SHM unit %d (struct size=%zu)\n",
         shmunit, sizeof(struct shmTime));
  printf("Initial PRU seq=%u\n", last_seq);

  while (running) {
    uint32_t seq = pru->seq;
    if (seq == last_seq) {
      struct timespec ts = {0, 50000};
      nanosleep(&ts, NULL);
      continue;
    }

    uint32_t pps_iep = pru->iep_lo;

    /*
     * Tight IEP-wall calibration: take 10 back-to-back samples,
     * keep the one with the smallest gap between the two
     * clock_gettime calls so we have the most accurate wall time
     * for the IEP counter value.
     */
    struct timespec t1, t2;
    long long best_spread = 999999999LL;
    uint32_t best_cal_iep = 0;
    long long best_cal_wall = 0;
    int i;
    for (i = 0; i < 10; i++) {
      clock_gettime(CLOCK_REALTIME, &t1);
      uint32_t c = iep[0x0C / 4];
      clock_gettime(CLOCK_REALTIME, &t2);
      long long ns1 = (long long)t1.tv_sec * 1000000000LL + t1.tv_nsec;
      long long ns2 = (long long)t2.tv_sec * 1000000000LL + t2.tv_nsec;
      long long spread = ns2 - ns1;
      if (spread < best_spread) {
        best_spread = spread;
        best_cal_iep = c;
        best_cal_wall = ns1 + spread / 2;
      }
    }

    last_seq = seq;

    if (have_prev) {
      uint32_t iep_delta = pps_iep - prev_pps_iep;
      if (iep_delta < 150000000U || iep_delta > 250000000U) {
        bad++;
        prev_pps_iep = pps_iep;
        continue;
      }
      double measured = 1e9 / (double)iep_delta;
      ns_per_tick = ns_per_tick * 0.9 + measured * 0.1;
      saved_delta = iep_delta;
    }

    prev_pps_iep = pps_iep;
    if (!have_prev) {
      have_prev = 1;
      printf("seq=%u pps_iep=%u (first pulse)\n", seq, pps_iep);
      continue;
    }

    good++;

    /*
     * Compute the wall-clock time of the PPS edge by projecting
     * back from the calibration point using the IEP tick rate.
     *
     * iep_offset is negative when pps_iep is earlier than
     * best_cal_iep (the normal case — the pulse happened before
     * we ran the calibration loop).
     */
    int32_t iep_offset = (int32_t)(pps_iep - best_cal_iep);
    long long pps_wall_ns =
        best_cal_wall + (long long)((double)iep_offset * ns_per_tick);

    /*
     * receiveTimeStamp = best estimate of wall time at the PPS edge.
     * This is what we actually measured.
     */
    long long rx_sec = pps_wall_ns / 1000000000LL;
    long rx_nsec = (long)(pps_wall_ns % 1000000000LL);
    if (rx_nsec < 0) {
      rx_sec--;
      rx_nsec += 1000000000L;
    }

    /*
     * clockTimeStamp = the true UTC second the pulse represents.
     * Round to nearest second — the PPS pulse nominally fires at
     * an exact second boundary so sub-second residual is just noise.
     */
    long long clock_sec = rx_sec;
    if (rx_nsec >= 500000000L)
      clock_sec++;

    /* offset for diagnostics */
    long offset_ns = rx_nsec;
    if (offset_ns > 500000000L)
      offset_ns -= 1000000000L;

    /*
     * Write SHM using mode-1 count handshake.
     * count must be ODD while we are writing, EVEN when done.
     */
    shm->valid = 0;
    __sync_synchronize();
    shm->count++; /* now odd  */

    shm->clockTimeStampSec = clock_sec;
    shm->clockTimeStampUSec = 0;
    shm->clockTimeStampNSec = 0;

    shm->receiveTimeStampSec = rx_sec;
    shm->receiveTimeStampUSec = (int32_t)(rx_nsec / 1000);
    shm->receiveTimeStampNSec = (uint32_t)rx_nsec;

    __sync_synchronize();
    shm->count++; /* now even */
    shm->valid = 1;

    if (good <= 10 || (good % 10) == 1) {
      uint32_t iep_gap = best_cal_iep - pps_iep;
      printf("seq=%u delta=%u offset=%+ld ns gap=%u (%.1f us) "
             "spread=%lld ns ns/tick=%.6f [good=%u]\n",
             seq, saved_delta, offset_ns, iep_gap,
             (double)iep_gap * ns_per_tick / 1000.0, best_spread, ns_per_tick,
             good);
    }
  }

  shm->valid = 0;
  printf("pru_pps_shm: exiting (good=%u bad=%u)\n", good, bad);
  return 0;
}
