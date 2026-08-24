/* pru_pps_shm.c
 * Userspace SHM bridge daemon for PRU PPS timestamping
 *
 * SPDX-License-Identifier: MIT-0
 * Copyright (c) 2026 dniminenn
 */

#define _GNU_SOURCE /* clock_adjtime */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/timex.h>
#include <time.h>
#include <sys/stat.h>
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
 * offset 52: unsigned clockTimeStampNSec (4)
 * offset 56: unsigned receiveTimeStampNSec (4)
 * offset 60: int dummy[8] (32)
 * total: 96 bytes (92 used, tail padded to the 8-byte alignment of time_t)
 *
 * The NSec pair follows `valid` with NO padding: both are 4-byte types, so
 * nothing needs aligning. An earlier version of this struct inserted a 4-byte
 * pad here, which pushed both NSec fields 4 bytes past where chrony reads
 * them. Chrony then saw clockTimeStampNSec = receiveTimeStampNSec = 0; that is
 * self-consistent with the USec fields whenever the offset is a small positive
 * number (0/1000 == 0), so chrony accepted them and measured an offset of
 * exactly zero, and fell back to USec resolution otherwise. The refclock was
 * therefore disciplined by a 1 us quantizer: 88858 logged samples, not one
 * with sub-microsecond content, while the true error sat at +435 ns. The
 * asserts below exist so that can never silently return.
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
  uint32_t clockTimeStampNSec;   /* 52 */
  uint32_t receiveTimeStampNSec; /* 56 */
  int32_t dummy[8];              /* 60 */
};

/*
 * These offsets are an ABI shared with chronyd/gpsd, not an implementation
 * detail. Verified empirically against gpsd's live SHM 0 on the same host:
 * clockTimeStampUSec=243 alongside clockTimeStampNSec=243230 at offset 52, and
 * receiveTimeStampUSec=216913 alongside receiveTimeStampNSec=216913865 at 56.
 */
_Static_assert(offsetof(struct shmTime, clockTimeStampSec) == 8, "shmTime ABI");
_Static_assert(offsetof(struct shmTime, clockTimeStampUSec) == 16, "shmTime ABI");
_Static_assert(offsetof(struct shmTime, receiveTimeStampSec) == 24, "shmTime ABI");
_Static_assert(offsetof(struct shmTime, receiveTimeStampUSec) == 32, "shmTime ABI");
_Static_assert(offsetof(struct shmTime, valid) == 48, "shmTime ABI");
_Static_assert(offsetof(struct shmTime, clockTimeStampNSec) == 52, "shmTime ABI");
_Static_assert(offsetof(struct shmTime, receiveTimeStampNSec) == 56, "shmTime ABI");
_Static_assert(sizeof(struct shmTime) == 96, "shmTime ABI");

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
#define TCXO_OFFSET 0x10     /* tcxo_shared follows pps_shared in PRU0 DRAM */
#define ECAP_BASE 0x4A330000 /* ICSS eCAP: TSCTR at 0, CAP1 latches PPS */
#define RPMSG_DEV "/dev/rpmsg_pru30"

/*
 * TCXO reference (optional): PRU0 also counts the DS3231's 32.768 kHz output
 * (P9_41A) and snapshots {edges, TSCTR} every 4096 edges. Consecutive
 * snapshots give the 200 MHz timebase's frequency against a ±2 ppm
 * temperature-compensated reference — a live crystal-drift measurement,
 * independent of GPS. Purely observational for now (logged + written to
 * /run/pps-tcxo-ppm); a frozen snapshot struct just disables it.
 */
struct tcxo_shared {
  volatile uint32_t seq;
  volatile uint32_t edges;
  volatile uint32_t tsctr;
};

/* ---- T2CXO: learned DS3231 tempco ---- */
#define T2_BINS 250 /* -40..85C, 0.5C bins */
#define T2_TEMP_MIN (-40.0)
#define T2_STATE "/var/lib/pru-pps-t2cxo.bin"
#define T2_MAGIC 0x54325831u
struct t2_state {
  uint32_t magic;
  float bin_ppm[T2_BINS];
  uint16_t bin_n[T2_BINS];
  float global_ppm;
  uint32_t global_n;
};
static struct t2_state t2;
static double t2_resid_ppm = 0.5; /* pessimistic until measured */
static double t2_last_e = 0, t2_last_corr = 0, t2_temp_c = -1000;
static char t2_hwmon[160];
static int64_t t2_gps_ticks = 0;
static uint64_t t2_gps_secs = 0;

/*
 * pruss PHC bookkeeping: the kernel PHC (ptp_pruss) counts the same eCAP
 * TSCTR this daemon reads, times 5 ns, from its own epoch. The constant K
 * between the two extended counts is learned by pairing a PHC gettime with a
 * bracketed TSCTR read; K in ticks never changes, so the residual is EWMAd
 * hard. With K known, the PHC's time at a PPS capture is exact (the capture
 * is hardware), and PHC minus the pulse's true second is the PHC's error
 * against GPS: the anchor that turns a free-running ptp4l offsetFromMaster
 * into a GM-vs-GPS measurement. Kept in int64: 2026 epoch nanoseconds round
 * at ~256 ns in a double.
 *
 * PHC_TAI_OFFSET_S: pruts.service sets the PHC to TAI at boot; 37 is the
 * stack-wide convention (phc2sys -O 37, chrony PHC offset -37).
 */
#define PHC_TAI_OFFSET_S 37
#define FD_TO_CLOCKID(fd) ((clockid_t)((((unsigned int)~(fd)) << 3) | 3))
static int phc_fd = -1;
static long long phc_off_ns;
static long long phc_anchor_mono_ns; /* CLOCK_MONOTONIC of the anchor pulse */
static int phc_off_valid;
static double phc_freq_ppb; /* measured d(PHC - true)/dt after trim */

/*
 * K(t) = PHC ns minus extended ticks * 5, sampled ~1 Hz by pairing a PHC
 * gettime with a bracketed TSCTR read. Constant while the PHC is untrimmed;
 * our own adjfine trims put a slope on it, so it is tracked as a windowed
 * line over MONOTONIC_RAW and the ring is flushed whenever the trim
 * changes. Values are huge (TAI epoch), so the fit works on deltas from
 * the window's first sample.
 */
#define KFIT_N 64
static struct kent { long long t, k; } kring[KFIT_N];
static int kring_n, kring_head;
static double kfit_a, kfit_b; /* k = a + b*(t - t0), b in ns/ns */
static long long kfit_t0, kfit_k0;
static int kfit_valid;

static void kring_reset(void) { kring_n = kring_head = kfit_valid = 0; }

static void kring_push(long long t, long long k) {
  kring[kring_head] = (struct kent){t, k};
  kring_head = (kring_head + 1) % KFIT_N;
  if (kring_n < KFIT_N)
    kring_n++;
  if (kring_n < 8) {
    kfit_valid = 0;
    return;
  }
  int base = (kring_head - kring_n + KFIT_N) % KFIT_N;
  kfit_t0 = kring[base].t;
  kfit_k0 = kring[base].k;
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (int j = 0; j < kring_n; j++) {
    struct kent *e = &kring[(base + j) % KFIT_N];
    double x = (double)(e->t - kfit_t0);
    double y = (double)(e->k - kfit_k0);
    sx += x; sy += y; sxx += x * x; sxy += x * y;
  }
  double det = kring_n * sxx - sx * sx;
  if (det <= 0)
    return;
  kfit_b = (kring_n * sxy - sx * sy) / det;
  kfit_a = (sy - kfit_b * sx) / kring_n;
  kfit_valid = 1;
}

static long long kfit_eval(long long t) {
  return kfit_k0 + (long long)(kfit_a + kfit_b * (double)(t - kfit_t0));
}

/* DS3231 absolute frequency estimate, from the tempco block */
static double phc_ds_err_ppb;
static long long phc_ds_mono_ns = -1;

/* last commanded trim */
static double phc_cmd_ppb;
static int phc_cmd_set;
static long long phc_last_pulse_mono = -1000000000000LL;

static void phc_open(void) {
  for (int i = 0; i < 8; i++) {
    char path[64], name[32];
    snprintf(path, sizeof(path), "/sys/class/ptp/ptp%d/clock_name", i);
    FILE *f = fopen(path, "r");
    if (!f)
      continue;
    if (fgets(name, sizeof(name), f) && !strncmp(name, "pruss-ecap", 10)) {
      fclose(f);
      snprintf(path, sizeof(path), "/dev/ptp%d", i);
      phc_fd = open(path, O_RDWR); /* adjfine trims need write */
      if (phc_fd >= 0)
        printf("pruss PHC found at %s\n", path);
      return;
    }
    fclose(f);
  }
  fprintf(stderr, "pru_pps_shm: no pruss-ecap PHC, phc offset export off\n");
}

static void t2_find_hwmon(void) {
  for (int i = 0; i < 10; i++) {
    char path[96], name[32];
    snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/name", i);
    FILE *f = fopen(path, "r");
    if (!f) continue;
    int hit = fgets(name, sizeof(name), f) && !strncmp(name, "ds3231", 6);
    fclose(f);
    if (hit) {
      snprintf(t2_hwmon, sizeof(t2_hwmon),
               "/sys/class/hwmon/hwmon%d/temp1_input", i);
      return;
    }
  }
}

static int t2_read_temp(double *c) {
  if (!t2_hwmon[0]) return 0;
  FILE *f = fopen(t2_hwmon, "r");
  if (!f) return 0;
  long m;
  int ok = fscanf(f, "%ld", &m) == 1;
  fclose(f);
  if (ok) *c = m / 1000.0;
  return ok;
}

static void t2_load(void) {
  FILE *f = fopen(T2_STATE, "r");
  if (!f) return;
  struct t2_state in;
  int ok = fread(&in, sizeof(in), 1, f) == 1 && in.magic == T2_MAGIC;
  fclose(f);
  if (ok && !(in.global_ppm > -50 && in.global_ppm < 50)) ok = 0;
  for (int i = 0; ok && i < T2_BINS; i++)
    if (in.bin_n[i] && !(in.bin_ppm[i] > -50 && in.bin_ppm[i] < 50)) ok = 0;
  if (ok) {
    t2 = in;
    printf("t2cxo: restored %u samples\n", t2.global_n);
  }
}

static void t2_save(void) {
  t2.magic = T2_MAGIC;
  FILE *f = fopen(T2_STATE ".tmp", "w");
  if (!f) return;
  int ok = fwrite(&t2, sizeof(t2), 1, f) == 1;
  if (fclose(f) == 0 && ok) rename(T2_STATE ".tmp", T2_STATE);
}

static int t2_corr(double *out) {
  if (t2_temp_c > -100) {
    int b = (int)((t2_temp_c - T2_TEMP_MIN) * 2.0);
    for (int r = 0; r <= 4; r++) { /* search outward to +-2C */
      for (int sgn = -1; sgn <= 1; sgn += 2) {
        int i = b + (sgn < 0 ? -r : r);
        if (i >= 0 && i < T2_BINS && t2.bin_n[i] >= 5) {
          *out = t2.bin_ppm[i];
          return 1;
        }
        if (r == 0) break;
      }
    }
  }
  if (t2.global_n >= 10) {
    *out = t2.global_ppm;
    return 1;
  }
  return 0;
}

static void t2_learn(double e) {
  if (e <= -50 || e >= 50) return; /* clamp implausible samples */
  double c;
  if (t2_corr(&c)) /* residual against pre-fold correction */
    t2_resid_ppm += 0.05 * (fabs(e - c) - t2_resid_ppm);
  if (t2.global_n == 0) t2.global_ppm = (float)e;
  else t2.global_ppm += 0.05f * ((float)e - t2.global_ppm);
  t2.global_n++;
  if (t2_temp_c > -100) {
    int b = (int)((t2_temp_c - T2_TEMP_MIN) * 2.0);
    if (b >= 0 && b < T2_BINS) {
      if (t2.bin_n[b] == 0) t2.bin_ppm[b] = (float)e;
      else t2.bin_ppm[b] += 0.2f * ((float)e - t2.bin_ppm[b]);
      if (t2.bin_n[b] < 65535) t2.bin_n[b]++;
    }
  }
}

/*
 * Two-stage transfer model: IEP -> MONOTONIC_RAW -> REALTIME.
 *
 * The original design fitted IEP directly against CLOCK_REALTIME. That created
 * a circular dependency: the daemon measured the PPS edge against the very
 * clock chrony steers using the daemon's own samples. Every correction chrony
 * applied perturbed the next measurement, so the refclock poll interval became
 * the damping of an unintended control loop — and speeding it up (poll 0
 * filter 4) drove it unstable in minutes: skew 0.006 -> 11.9 ppm, root
 * dispersion 3 -> 56 us, pulse offsets swinging +-16 us.
 *
 * Splitting the model removes the feedback from the precision-critical term:
 *
 *   fit A: IEP <-> CLOCK_MONOTONIC_RAW    hardware to hardware. RAW is never
 *          adjusted by NTP (unlike CLOCK_MONOTONIC, which is frequency
 *          corrected), so this relation is genuinely linear with no kinks to
 *          extrapolate across. Long window, and the noisy term lives here:
 *          reading the IEP is an uncached OCP access costing microseconds,
 *          which averaging over many samples suppresses.
 *
 *   fit B: MONOTONIC_RAW <-> (REALTIME - MONOTONIC_RAW)   i.e. chrony's own
 *          cumulative adjustment, and nothing else. Short window, because this
 *          must TRACK corrections rather than average them away. Its samples
 *          are two back-to-back clock_gettime() calls (vDSO, cheap and tight),
 *          so a short window is affordable here.
 *
 * The pulse is placed by evaluating fit A at the latched tick, then adding
 * fit B at that instant. Because fit A is linear, evaluating it slightly past
 * its newest sample is harmless, so the old "hold each pulse for half a window
 * then interpolate" trick is gone — which also removes ~0.5 s of delay from
 * inside chrony's servo loop.
 *
 * MONOTONIC_RAW plays the role a NIC PHC plays on a machine that disciplines
 * a separate hardware clock and then steers the system clock from it: a
 * reference the consumer of the measurement never perturbs. The AM335x CPTS
 * could serve that role literally (it exposes 4 extts channels), but it has no
 * PTP_SYS_OFFSET_PRECISE, so reading it costs tens of microseconds with
 * microsecond-scale scatter - far worse than this.
 */
#define CAL_WIN 512               /* fit A ring: IEP <-> MONOTONIC_RAW      */
#define ADJ_WIN 512               /* fit B ring: chrony's adjustment        */
/*
 * Extra (RAW,REALTIME) pairs per loop iteration. What reaches the reported
 * offset is fit B's prediction error at the pulse, ~rms/sqrt(n) times the
 * edge leverage -- so n is the lever, not per-sample noise (tightening the
 * bracket gate was tried and lost more samples than it gained precision).
 * Density is raised inside the existing 2 s horizon rather than by widening
 * it, because REALTIME-MONOTONIC_RAW slews at ~53ppm and a longer window
 * curves away from the straight line being fitted.
 */
#define ADJ_EXTRA_SAMPLES 3
#define CAL_INTERVAL_MS 25
#define CAL_SPREAD_GATE_NS 2000   /* accept an IEP bracket this tight       */
#define ADJ_SPREAD_GATE_NS 4000   /* 3 clock_gettime calls; each costs ~1us   */
#define CAL_MIN_SAMPLES 24
#define ADJ_MIN_SAMPLES 8
#define CAL_SPAN_NS 8000000000LL  /* fit A horizon: 8 s of linear relation  */
#define ADJ_SPAN_NS 2000000000LL  /* fit B horizon: 2 s, tracks corrections */
#define ADJ_STEP_GATE_NS 10000    /* residual beyond this = clock stepped   */

struct cal_sample {
  int64_t iep;    /* extended (unwrapped) IEP ticks */
  long long mono; /* CLOCK_MONOTONIC_RAW ns at that tick */
};

struct adj_sample {
  long long mono; /* CLOCK_MONOTONIC_RAW ns */
  long long adj;  /* REALTIME - MONOTONIC_RAW at that instant */
};

struct cal_fit {
  int n;
  double slope;        /* ns per tick (fit A) or ns/ns (fit B) */
  double x0, y0;       /* centroid, relative to the bases below */
  int64_t xbase;
  long long ybase;
  double rms;
  int valid;
};

static struct cal_sample cal_ring[CAL_WIN];
static int cal_head = 0, cal_count = 0;
static struct adj_sample adj_ring[ADJ_WIN];
static int adj_head = 0, adj_count = 0;

/*
 * A CLOCK_REALTIME step invalidates only fit B, which models
 * REALTIME - MONOTONIC_RAW. Fit A is IEP against MONOTONIC_RAW - hardware
 * against hardware - and cannot be affected by anything chrony does to
 * REALTIME. Flushing it on a step threw away a converged 512-sample window
 * (n~370, 7ns residual) and restarted at n~25 with a ~4x noisier slope, which
 * fed bad pulses to chrony, which corrected harder, which tripped this gate
 * again. Reset the two fits independently.
 */
static void adj_reset(void) {
  adj_head = adj_count = 0;
}

static void cal_push(int64_t iep, long long mono) {
  cal_ring[cal_head] = (struct cal_sample){iep, mono};
  cal_head = (cal_head + 1) % CAL_WIN;
  if (cal_count < CAL_WIN)
    cal_count++;
}

static void adj_push(long long mono, long long adj) {
  adj_ring[adj_head] = (struct adj_sample){mono, adj};
  adj_head = (adj_head + 1) % ADJ_WIN;
  if (adj_count < ADJ_WIN)
    adj_count++;
}

/* fit A: least squares MONOTONIC_RAW = f(IEP) over the trailing CAL_SPAN_NS. */
static void cal_fit_iep(struct cal_fit *f) {
  memset(f, 0, sizeof(*f));
  if (cal_count < CAL_MIN_SAMPLES)
    return;
  int idx = (cal_head + CAL_WIN - cal_count) % CAL_WIN;
  const struct cal_sample *newest = &cal_ring[(cal_head + CAL_WIN - 1) % CAL_WIN];
  int64_t xb = newest->iep;
  long long yb = newest->mono;
  double sx = 0, sy = 0;
  int n = 0;
  for (int i = 0; i < cal_count; i++) {
    const struct cal_sample *s = &cal_ring[(idx + i) % CAL_WIN];
    if (newest->mono - s->mono > CAL_SPAN_NS)
      continue;
    sx += (double)(s->iep - xb);
    sy += (double)(s->mono - yb);
    n++;
  }
  if (n < CAL_MIN_SAMPLES)
    return;
  double mx = sx / n, my = sy / n, sxx = 0, sxy = 0;
  for (int i = 0; i < cal_count; i++) {
    const struct cal_sample *s = &cal_ring[(idx + i) % CAL_WIN];
    if (newest->mono - s->mono > CAL_SPAN_NS)
      continue;
    double dx = (double)(s->iep - xb) - mx;
    double dy = (double)(s->mono - yb) - my;
    sxx += dx * dx;
    sxy += dx * dy;
  }
  if (sxx <= 0)
    return;
  double b = sxy / sxx;
  double rss = 0;
  for (int i = 0; i < cal_count; i++) {
    const struct cal_sample *s = &cal_ring[(idx + i) % CAL_WIN];
    if (newest->mono - s->mono > CAL_SPAN_NS)
      continue;
    double r = ((double)(s->mono - yb) - my) - b * ((double)(s->iep - xb) - mx);
    rss += r * r;
  }
  f->n = n;
  f->slope = b;
  f->x0 = mx;
  f->y0 = my;
  f->xbase = xb;
  f->ybase = yb;
  f->rms = sqrt(rss / n);
  /* ~5 ns per tick at 200 MHz; RAW is unsteered so this must be very stable */
  f->valid = (b > 4.9 && b < 5.1 && f->rms < 2000.0);
}

/* fit B: least squares adj = f(MONOTONIC_RAW) over the trailing ADJ_SPAN_NS. */
static void cal_fit_adj(struct cal_fit *f) {
  memset(f, 0, sizeof(*f));
  if (adj_count < ADJ_MIN_SAMPLES)
    return;
  int idx = (adj_head + ADJ_WIN - adj_count) % ADJ_WIN;
  const struct adj_sample *newest = &adj_ring[(adj_head + ADJ_WIN - 1) % ADJ_WIN];
  long long xb = newest->mono, yb = newest->adj;
  double sx = 0, sy = 0;
  int n = 0;
  for (int i = 0; i < adj_count; i++) {
    const struct adj_sample *s = &adj_ring[(idx + i) % ADJ_WIN];
    if (newest->mono - s->mono > ADJ_SPAN_NS)
      continue;
    sx += (double)(s->mono - xb);
    sy += (double)(s->adj - yb);
    n++;
  }
  if (n < ADJ_MIN_SAMPLES)
    return;
  double mx = sx / n, my = sy / n, sxx = 0, sxy = 0;
  for (int i = 0; i < adj_count; i++) {
    const struct adj_sample *s = &adj_ring[(idx + i) % ADJ_WIN];
    if (newest->mono - s->mono > ADJ_SPAN_NS)
      continue;
    double dx = (double)(s->mono - xb) - mx;
    double dy = (double)(s->adj - yb) - my;
    sxx += dx * dx;
    sxy += dx * dy;
  }
  double b = (sxx > 0) ? sxy / sxx : 0.0;   /* slope = chrony's slew rate */
  double rss = 0;
  for (int i = 0; i < adj_count; i++) {
    const struct adj_sample *s = &adj_ring[(idx + i) % ADJ_WIN];
    if (newest->mono - s->mono > ADJ_SPAN_NS)
      continue;
    double r = ((double)(s->adj - yb) - my) - b * ((double)(s->mono - xb) - mx);
    rss += r * r;
  }
  f->n = n;
  f->slope = b;
  f->x0 = mx;
  f->y0 = my;
  f->xbase = xb;
  f->ybase = yb;
  f->rms = sqrt(rss / n);
  /* chrony slews at ppm scale; anything past 1000 ppm means a step happened */
  f->valid = (b > -1e-3 && b < 1e-3);
}

static long long cal_eval(const struct cal_fit *f, int64_t x) {
  double rel = f->y0 + f->slope * ((double)(x - f->xbase) - f->x0);
  return f->ybase + (long long)rel;
}


/*
 * UBX-TIM-TP sawtooth correction.
 *
 * The receiver can only place the timepulse edge on its own clock grid; the
 * sub-grid error (up to ~±10 ns on an M8N) is reported per pulse as qErr in
 * UBX-TIM-TP, which also carries the GPS week/tow of the pulse it describes.
 * We take the raw UBX stream as a read-only gpsd client (?WATCH raw=2 — gpsd
 * keeps owning the tty) and pair each TIM-TP to its pulse by converting
 * week/tow to the UTC second and matching against the pulse's rounded second.
 * A small history ring covers the ~2 s evaluation delay. qErr sign convention
 * is applied via qerr_sign (see -q); 0 logs without correcting, for
 * empirically confirming the convention against the measured offsets.
 */
#define GPSD_HOST "127.0.0.1"
#define GPSD_PORT 2947
#define GPS_EPOCH_UNIX 315964800LL
#define GPS_UTC_LEAP_S 18 /* GPS-UTC offset; only used when timeBase=GPS */
#define QHIST 8

static struct {
  long long sec;
  double ns;
} qhist[QHIST];
static int qhead = 0;

static void ubx_timtp(const uint8_t *p) {
  uint32_t tow_ms = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  int32_t qerr_ps = (int32_t)((uint32_t)p[8] | ((uint32_t)p[9] << 8) |
                              ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24));
  uint16_t week = (uint16_t)p[12] | ((uint16_t)p[13] << 8);
  uint8_t flags = p[14];
  long long t = GPS_EPOCH_UNIX + (long long)week * 604800LL + tow_ms / 1000;
  if (!(flags & 1)) /* timeBase 0 = GPS: convert to UTC */
    t -= GPS_UTC_LEAP_S;
  qhist[qhead].sec = t;
  qhist[qhead].ns = (double)qerr_ps / 1000.0;
  qhead = (qhead + 1) % QHIST;
}

static int qerr_lookup(long long sec, double *ns) {
  for (int i = 0; i < QHIST; i++) {
    if (qhist[i].sec == sec) {
      *ns = qhist[i].ns;
      return 1;
    }
  }
  return 0;
}

/* Byte-stream UBX frame scanner; ignores the interleaved NMEA text. */
static void ubx_feed(const uint8_t *buf, ssize_t len) {
  static int st = 0;
  static uint8_t cls, id, ck_a, ck_b;
  static uint16_t plen, pgot;
  static uint8_t payload[64];
  for (ssize_t i = 0; i < len; i++) {
    uint8_t c = buf[i];
    switch (st) {
    case 0: st = (c == 0xB5) ? 1 : 0; break;
    case 1: st = (c == 0x62) ? 2 : 0; break;
    case 2: cls = c; ck_a = c; ck_b = c; st = 3; break;
    case 3: id = c; ck_a += c; ck_b += ck_a; st = 4; break;
    case 4: plen = c; ck_a += c; ck_b += ck_a; st = 5; break;
    case 5:
      plen |= (uint16_t)c << 8;
      ck_a += c; ck_b += ck_a;
      pgot = 0;
      st = (plen > sizeof(payload)) ? 8 : (plen ? 6 : 7);
      break;
    case 6:
      payload[pgot++] = c;
      ck_a += c; ck_b += ck_a;
      if (pgot == plen) st = 7;
      break;
    case 7: /* ck_a */
      st = (c == ck_a) ? 9 : 0;
      break;
    case 8: /* oversized frame: consume payload without storing */
      ck_a += c; ck_b += ck_a;
      if (++pgot == plen) st = 7;
      break;
    case 9: /* ck_b */
      if (c == ck_b && cls == 0x0D && id == 0x01 && plen == 16)
        ubx_timtp(payload);
      st = 0;
      break;
    }
  }
}

static int gpsd_connect(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_port = htons(GPSD_PORT);
  inet_pton(AF_INET, GPSD_HOST, &a.sin_addr);
  if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
    close(fd);
    return -1;
  }
  const char *watch = "?WATCH={\"enable\":true,\"raw\":2};\n";
  if (write(fd, watch, strlen(watch)) < 0) {
    close(fd);
    return -1;
  }
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  return fd;
}

/* Pulses waiting for their centered window to mature (~2 s). */
#define PEND_MAX 8
struct pending {
  int64_t pps_ext;
  uint32_t seq;
};
static struct pending pend[PEND_MAX];
static int pend_head = 0, pend_count = 0;


/*
 * Publish servo state as a Prometheus textfile for the metrics exporter to
 * merge. Written to tmpfs and renamed into place so a scrape never sees a
 * partial file. Called from the existing report point so it adds no new I/O
 * cadence to the capture loop.
 */
#define PROM_PATH "/run/pruts/iep.prom"
static void write_prom(long offset_ns, double iep_rms, double iep_slope,
                       double adj_rms, double adj_slew, double qerr,
                       unsigned good, unsigned bad, unsigned dropped) {
  char tmp[] = PROM_PATH ".XXXXXX";
  int fd = mkstemp(tmp);
  if (fd < 0) return;
  (void)fchmod(fd, 0644);
  FILE* f = fdopen(fd, "w");
  if (!f) { close(fd); unlink(tmp); return; }
  fprintf(f,
    "# HELP pru_pps_offset_ns PPS offset against the capture clock, nanoseconds\n"
    "# TYPE pru_pps_offset_ns gauge\n"
    "pru_pps_offset_ns{clock=\"iep0\"} %ld\n"
    "# HELP pru_pps_freq_ppb Capture-clock frequency error, parts per billion\n"
    "# TYPE pru_pps_freq_ppb gauge\n"
    "pru_pps_freq_ppb{clock=\"iep0\"} %.1f\n"
    "# HELP pps_fit_rms_ns Least-squares residual of each transfer fit\n"
    "# TYPE pps_fit_rms_ns gauge\n"
    "pps_fit_rms_ns{fit=\"iep\"} %.1f\n"
    "pps_fit_rms_ns{fit=\"adj\"} %.1f\n"
    "# HELP pps_fit_slope Fitted slope of each transfer fit\n"
    "# TYPE pps_fit_slope gauge\n"
    "pps_fit_slope{fit=\"iep\"} %.9f\n"
    "# HELP pps_timepulse_quantization_error_ns Receiver sawtooth correction\n"
    "# TYPE pps_timepulse_quantization_error_ns gauge\n"
    "pps_timepulse_quantization_error_ns %.1f\n"
    "# HELP pps_pulses_total PPS pulses by disposition\n"
    "# TYPE pps_pulses_total counter\n"
    "pps_pulses_total{disposition=\"good\"} %u\n"
    "pps_pulses_total{disposition=\"bad\"} %u\n"
    "pps_pulses_total{disposition=\"dropped\"} %u\n",
    offset_ns, adj_slew * 1e9, iep_rms, adj_rms, iep_slope, qerr,
    good, bad, dropped);
  fprintf(f,
    "# HELP t2cxo_err_ppm DS3231 error vs GPS, last window\n"
    "# TYPE t2cxo_err_ppm gauge\n"
    "t2cxo_err_ppm %.4f\n"
    "# HELP t2cxo_corr_ppm Learned correction in use\n"
    "# TYPE t2cxo_corr_ppm gauge\n"
    "t2cxo_corr_ppm %.4f\n"
    "# HELP t2cxo_residual_ppm EWMA error of the learned correction\n"
    "# TYPE t2cxo_residual_ppm gauge\n"
    "t2cxo_residual_ppm %.4f\n"
    "# HELP t2cxo_samples Learned 60s windows\n"
    "# TYPE t2cxo_samples counter\n"
    "t2cxo_samples %u\n"
    "# HELP ds3231_temp_celsius DS3231 die temperature\n"
    "# TYPE ds3231_temp_celsius gauge\n"
    "ds3231_temp_celsius %.2f\n",
    t2_last_e, t2_last_corr, t2_resid_ppm, t2.global_n, t2_temp_c);
  if (phc_off_valid)
    fprintf(f,
      "# HELP pruss_phc_offset_ns pruss PHC minus GPS time (TAI) at the PPS capture\n"
      "# TYPE pruss_phc_offset_ns gauge\n"
      "pruss_phc_offset_ns %lld\n"
      "# HELP pruss_phc_anchor_mono_ns CLOCK_MONOTONIC of the anchor pulse\n"
      "# TYPE pruss_phc_anchor_mono_ns gauge\n"
      "pruss_phc_anchor_mono_ns %lld\n"
      "# HELP pruss_phc_freq_ppb PHC rate error vs GPS after the trim\n"
      "# TYPE pruss_phc_freq_ppb gauge\n"
      "pruss_phc_freq_ppb %.1f\n",
      phc_off_ns, phc_anchor_mono_ns, phc_freq_ppb);
  if (fclose(f) != 0) { unlink(tmp); return; }
  if (rename(tmp, PROM_PATH) != 0) unlink(tmp);
}

int main(int argc, char **argv) {
  int shmunit = 2;
  const char *rpmsg_dev = RPMSG_DEV;
  int qerr_sign = 0; /* -1/+1 apply, 0 = log-only (sign experiment) */
  int opt;

  /* antenna + capture chain delay; chrony.conf compensates the same
   * constant on the SHM refclock (offset -0.0000047), recalibrate both
   * together */
  long pps_delay_ns = 4700;

  while ((opt = getopt(argc, argv, "s:r:q:o:")) != -1) {
    switch (opt) {
    case 's':
      shmunit = atoi(optarg);
      break;
    case 'r':
      rpmsg_dev = optarg;
      break;
    case 'q':
      qerr_sign = atoi(optarg);
      break;
    case 'o':
      pps_delay_ns = atol(optarg);
      break;
    default:
      fprintf(stderr,
              "Usage: %s [-s shmunit] [-r rpmsg_dev] [-q -1|0|1] "
              "[-o pps_delay_ns]\n",
              argv[0]);
      return 1;
    }
  }

  setlinebuf(stdout);
  signal(SIGINT, sighandler);
  signal(SIGTERM, sighandler);

  /* Open rpmsg char device for blocking PPS notifications */
  int rpmsg_fd = open(rpmsg_dev, O_RDWR);
  if (rpmsg_fd < 0) {
    perror(rpmsg_dev);
    return 1;
  }
  /* Send a setup byte to register our endpoint with the PRU */
  if (write(rpmsg_fd, "S", 1) < 0) {
    perror("rpmsg setup write");
    return 1;
  }

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
      NULL, 0x1000, PROT_READ, MAP_SHARED, memfd, ECAP_BASE);
  if (iep == MAP_FAILED) {
    perror("mmap ECAP");
    return 1;
  }
  volatile struct tcxo_shared *tcxo =
      (volatile struct tcxo_shared *)((volatile uint8_t *)pru + TCXO_OFFSET);
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
  int64_t prev_pps_ext = 0;
  int have_prev = 0;
  uint32_t good = 0, bad = 0, dropped = 0;

  /*
   * Extended IEP counter: the hardware register is 32-bit and wraps every
   * ~21.5 s at 200 MHz. All raw reads funnel through this single-threaded
   * unwrapper; signed 32-bit deltas place any tick within ±10.7 s of the
   * last committed read, which the 25 ms sampling cadence guarantees.
   */
  int64_t iep_ext = 0;
  uint32_t iep_last_raw = iep[0] /* eCAP TSCTR */;
  int step_strikes = 0;

  printf("pru_pps_shm: blocking on %s -> SHM unit %d (struct size=%zu, "
         "qerr_sign=%d)\n",
         rpmsg_dev, shmunit, sizeof(struct shmTime), qerr_sign);
  printf("Initial PRU seq=%u\n", last_seq);

  t2_find_hwmon();
  t2_load();
  phc_open();

  char rpmsg_buf[32];
  struct cal_fit iepfit = {0}, adjfit = {0};
  int gpsd_fd = gpsd_connect();
  long long gpsd_retry_ns = 0;
  if (gpsd_fd < 0)
    fprintf(stderr,
            "pru_pps_shm: gpsd not reachable yet — qErr disabled until it is\n");

  while (running) {
    struct pollfd pfds[2] = {
        {.fd = rpmsg_fd, .events = POLLIN},
        {.fd = gpsd_fd, .events = POLLIN},
    };
    int pret = poll(pfds, gpsd_fd >= 0 ? 2 : 1, CAL_INTERVAL_MS);
    if (pret < 0) {
      if (errno == EINTR)
        continue;
      perror("poll");
      break;
    }

    /* drain gpsd raw stream through the UBX scanner */
    if (gpsd_fd >= 0 && (pfds[1].revents & (POLLIN | POLLERR | POLLHUP))) {
      uint8_t gbuf[2048];
      ssize_t gn = read(gpsd_fd, gbuf, sizeof(gbuf));
      if (gn > 0) {
        ubx_feed(gbuf, gn);
      } else if (gn == 0 || (gn < 0 && errno != EAGAIN)) {
        close(gpsd_fd);
        gpsd_fd = -1;
        fprintf(stderr, "pru_pps_shm: lost gpsd — will reconnect\n");
      }
    }
    if (gpsd_fd < 0) {
      struct timespec tnow;
      clock_gettime(CLOCK_MONOTONIC, &tnow);
      long long mono = (long long)tnow.tv_sec * 1000000000LL + tnow.tv_nsec;
      if (mono - gpsd_retry_ns > 5000000000LL) {
        gpsd_retry_ns = mono;
        gpsd_fd = gpsd_connect();
        if (gpsd_fd >= 0)
          fprintf(stderr, "pru_pps_shm: gpsd reconnected\n");
      }
    }

    /*
     * TCXO ratio bookkeeping: PRU1 publishes a new snapshot every 125 ms;
     * accumulate extended TSCTR/edge counts and report the timebase's
     * frequency against the DS3231 every ~60 s. Wrap-safe: consecutive
     * snapshots are far inside the 21.5 s TSCTR wrap.
     */
    static uint32_t tq_seq = 0, tq_edges = 0, tq_tsctr = 0;
    static int64_t tq_tsctr_ext = 0, tq_ref_ext = 0;
    static uint64_t tq_edges_tot = 0, tq_ref_edges = 0;
    static int tq_have = 0;
    if (tcxo) {
      uint32_t s1 = tcxo->seq;
      if (s1 != tq_seq) {
        uint32_t e = tcxo->edges, ts = tcxo->tsctr;
        if (tcxo->seq == s1) { /* fields settled */
          if (tq_have) {
            tq_tsctr_ext += (int32_t)(ts - tq_tsctr);
            tq_edges_tot += (uint32_t)(e - tq_edges);
          } else {
            tq_have = 1;
            tq_ref_ext = tq_tsctr_ext;
            tq_ref_edges = tq_edges_tot;
          }
          tq_seq = s1;
          tq_edges = e;
          tq_tsctr = ts;
          if (tq_edges_tot - tq_ref_edges >= 32768ULL * 60) {
            double sec_tcxo = (double)(tq_edges_tot - tq_ref_edges) / 32768.0;
            double hz = (double)(tq_tsctr_ext - tq_ref_ext) / sec_tcxo;
            double ppm = (hz / 200e6 - 1.0) * 1e6;
            /*
             * chrony's tempcomp rejects values beyond ±10 ppm, and rightly
             * so — the servo/driftfile own the constant part of the crystal
             * error. The TCXO contributes only the DEVIATION from the first
             * window measured after daemon start; chrony re-absorbs the
             * baseline into its drift estimate on every restart.
             */
            /* learn TCXO error when GPS covered window */
            if (!t2_read_temp(&t2_temp_c))
              t2_temp_c = -1000;
            static int64_t t2_ref_ticks;
            static uint64_t t2_ref_secs;
            static int t2_win_have = 0;
            static uint32_t t2_wins = 0, t2_saved_n = 0;
            uint64_t t2_dsec = t2_gps_secs - t2_ref_secs;
            int64_t t2_dticks = t2_gps_ticks - t2_ref_ticks;
            if (t2_win_have && (double)t2_dsec > 0.9 * sec_tcxo &&
                (double)t2_dsec < 1.1 * sec_tcxo) {
              double gps_ppm =
                  ((double)t2_dticks / (double)t2_dsec / 200e6 - 1.0) * 1e6;
              t2_last_e = ppm - gps_ppm;
              t2_learn(t2_last_e);
            }
            t2_ref_ticks = t2_gps_ticks;
            t2_ref_secs = t2_gps_secs;
            t2_win_have = 1;
            double t2_c = 0;
            t2_last_corr = t2_corr(&t2_c) ? t2_c : 0;
            /* corrected: TCXO error learned out */
            double corrected = ppm - t2_last_corr;
            {
              struct timespec dm;
              clock_gettime(CLOCK_MONOTONIC, &dm);
              phc_ds_err_ppb = corrected * 1000.0;
              phc_ds_mono_ns =
                  (long long)dm.tv_sec * 1000000000LL + dm.tv_nsec;
            }
            static double base_ppm;
            static int have_base = 0;
            if (!have_base) {
              base_ppm = corrected;
              have_base = 1;
            }
            printf("tcxo: timebase %+0.3f ppm vs DS3231, e %+0.3f corr %+0.3f "
                   "resid %.3f n=%u temp %.2f dev %+0.3f (%.0f s window)\n",
                   ppm, t2_last_e, t2_last_corr, t2_resid_ppm, t2.global_n,
                   t2_temp_c, corrected - base_ppm, sec_tcxo);
            FILE *pf = fopen("/run/pps-tcxo-ppm.tmp", "w");
            if (pf) {
              fprintf(pf, "%.3f\n", corrected - base_ppm);
              fclose(pf);
              rename("/run/pps-tcxo-ppm.tmp", "/run/pps-tcxo-ppm");
            }
            if (++t2_wins % 60 == 0 && t2.global_n >= t2_saved_n + 10) {
              t2_save(); /* hourly, if learning */
              t2_saved_n = t2.global_n;
            }
            tq_ref_ext = tq_tsctr_ext;
            tq_ref_edges = tq_edges_tot;
          }
        }
      }
    }

    /*
     * Sampler. Two independent brackets per iteration:
     *
     *   fit A pair: MONOTONIC_RAW / IEP / MONOTONIC_RAW. The IEP read is an
     *   uncached OCP access bracketing at 1.5-3 us on this A8, so keep the
     *   tightest of three.
     *
     *   fit B pair: MONOTONIC_RAW / REALTIME / MONOTONIC_RAW. Both are vDSO
     *   reads and bracket far tighter, so one attempt is enough.
     */
    struct timespec t1, t2, t3;
    long long m1 = 0, m2 = 0;
    long long best_bracket = LLONG_MAX;
    uint32_t raw = 0;
    for (int k = 0; k < 3; k++) {
      clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
      uint32_t c = iep[0] /* eCAP TSCTR */;
      clock_gettime(CLOCK_MONOTONIC_RAW, &t2);
      long long a = (long long)t1.tv_sec * 1000000000LL + t1.tv_nsec;
      long long b = (long long)t2.tv_sec * 1000000000LL + t2.tv_nsec;
      if (b - a < best_bracket) {
        best_bracket = b - a;
        m1 = a;
        m2 = b;
        raw = c;
      }
    }
    iep_ext += (int32_t)(raw - iep_last_raw);
    iep_last_raw = raw;
    if (m2 - m1 < CAL_SPREAD_GATE_NS)
      cal_push(iep_ext, m1 + (m2 - m1) / 2);

    /* pair the kernel PHC with a bracketed TSCTR read, ~1 Hz */
    static int phc_tick = 0;
    if (phc_fd >= 0 && ++phc_tick >= 40) {
      phc_tick = 0;
      struct timespec pt, pm1, pm2;
      clock_gettime(CLOCK_MONOTONIC_RAW, &pm1);
      uint32_t pr1 = iep[0];
      if (clock_gettime(FD_TO_CLOCKID(phc_fd), &pt) == 0) {
        uint32_t pr2 = iep[0];
        clock_gettime(CLOCK_MONOTONIC_RAW, &pm2);
        int64_t e1 = iep_ext + (int32_t)(pr1 - iep_last_raw);
        int64_t e2 = iep_ext + (int32_t)(pr2 - iep_last_raw);
        long long pt_ns = (long long)pt.tv_sec * 1000000000LL + pt.tv_nsec;
        long long tp =
            ((long long)pm1.tv_sec * 1000000000LL + pm1.tv_nsec +
             (long long)pm2.tv_sec * 1000000000LL + pm2.tv_nsec) / 2;
        long long k = pt_ns - (e1 + e2) / 2 * 5;
        if (kfit_valid && fabs((double)(k - kfit_eval(tp))) > 1e6) {
          kring_reset(); /* phc stepped */
          phc_off_valid = 0;
        }
        kring_push(tp, k);

        /* trim the PHC frequency, ~1/min: GPS while locked, DS3231 in
         * holdover */
        static int trim_ctr = 0;
        if (++trim_ctr >= 64) {
          trim_ctr = 0;
          double target = 0;
          const char *src = NULL;
          long long now_mono;
          struct timespec tn;
          clock_gettime(CLOCK_MONOTONIC, &tn);
          now_mono = (long long)tn.tv_sec * 1000000000LL + tn.tv_nsec;
          if (adjfit.valid &&
              now_mono - phc_last_pulse_mono < 30000000000LL) {
            target = adjfit.slope * 1e9;
            src = "gps";
          } else if (phc_ds_mono_ns >= 0 &&
                     now_mono - phc_ds_mono_ns < 600000000000LL) {
            target = -phc_ds_err_ppb;
            src = "ds3231";
          }
          if (src && (!phc_cmd_set || fabs(target - phc_cmd_ppb) > 5)) {
            struct timex tx = {0};
            tx.modes = ADJ_FREQUENCY;
            tx.freq = (long)llround(target * 65.536);
            if (clock_adjtime(FD_TO_CLOCKID(phc_fd), &tx) != -1) {
              phc_cmd_ppb = target;
              phc_cmd_set = 1;
              kring_reset(); /* slope knee */
              printf("phc trim %+.0f ppb (%s)\n", target, src);
            } else {
              perror("clock_adjtime(phc)");
            }
          }
        }
      }
    }

    /* chrony's cumulative adjustment, REALTIME - MONOTONIC_RAW */
    clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
    clock_gettime(CLOCK_REALTIME, &t3);
    clock_gettime(CLOCK_MONOTONIC_RAW, &t2);
    {
      long long a = (long long)t1.tv_sec * 1000000000LL + t1.tv_nsec;
      long long b = (long long)t2.tv_sec * 1000000000LL + t2.tv_nsec;
      long long rt = (long long)t3.tv_sec * 1000000000LL + t3.tv_nsec;
      if (b - a < ADJ_SPREAD_GATE_NS) {
        long long mono_mid = a + (b - a) / 2;
        /*
         * Step detection lives here now, where it belongs: a chrony step is a
         * discontinuity in the adjustment, and fit B is the only thing that
         * models it. Three consecutive wild residuals flush both rings.
         */
        if (adjfit.valid) {
          long long pred = cal_eval(&adjfit, mono_mid);
          if (llabs((rt - mono_mid) - pred) > ADJ_STEP_GATE_NS) {
            if (++step_strikes >= 3) {
              fprintf(stderr,
                      "pru_pps_shm: clock step detected - resetting adjustment fit\n");
              adj_reset();
              step_strikes = 0;
            }
          } else {
            step_strikes = 0;
          }
        }
        adj_push(mono_mid, rt - mono_mid);
      }
    }

    /* Additional pairs, same horizon: raises n without widening the window.
     * Step detection deliberately stays on the first sample only, so strike
     * accounting is unchanged. */
    for (int k = 0; k < ADJ_EXTRA_SAMPLES; ++k) {
      struct timespec e1, e2, e3;
      clock_gettime(CLOCK_MONOTONIC_RAW, &e1);
      clock_gettime(CLOCK_REALTIME, &e3);
      clock_gettime(CLOCK_MONOTONIC_RAW, &e2);
      long long ea = (long long)e1.tv_sec * 1000000000LL + e1.tv_nsec;
      long long eb = (long long)e2.tv_sec * 1000000000LL + e2.tv_nsec;
      long long er = (long long)e3.tv_sec * 1000000000LL + e3.tv_nsec;
      if (eb - ea < ADJ_SPREAD_GATE_NS) {
        long long emid = ea + (eb - ea) / 2;
        adj_push(emid, er - emid);
      }
    }
    cal_fit_iep(&iepfit);
    cal_fit_adj(&adjfit);

    /*
     * Place any pending pulse. No waiting for a window to mature: fit A is a
     * hardware-to-hardware relation with no kinks, so evaluating it at an edge
     * a few ms behind its newest sample is as good as interpolating.
     */
    while (pend_count) {
      struct pending *p = &pend[pend_head];
      pend_head = (pend_head + 1) % PEND_MAX;
      pend_count--;
      if (!iepfit.valid || !adjfit.valid) {
        dropped++;   /* rings still filling, or a step just flushed them */
        continue;
      }
      /* edge on the unsteered timeline, then chrony's adjustment at that
       * instant: REALTIME = MONOTONIC_RAW + adj */
      long long mono_edge = cal_eval(&iepfit, p->pps_ext);
      long long pps_wall_ns = mono_edge + cal_eval(&adjfit, mono_edge);

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

      {
        struct timespec mn, mr;
        clock_gettime(CLOCK_MONOTONIC, &mn);
        clock_gettime(CLOCK_MONOTONIC_RAW, &mr);
        long long mn_ns = (long long)mn.tv_sec * 1000000000LL + mn.tv_nsec;
        long long mr_ns = (long long)mr.tv_sec * 1000000000LL + mr.tv_nsec;
        phc_last_pulse_mono = mn_ns;
        if (kfit_valid) {
          long long cap_ns = p->pps_ext * 5 + kfit_eval(mono_edge);
          phc_off_ns = cap_ns - (clock_sec + PHC_TAI_OFFSET_S) * 1000000000LL -
                       pps_delay_ns;
          /* journald stamps with slewed MONOTONIC; rebase the RAW edge */
          phc_anchor_mono_ns = mn_ns - (mr_ns - mono_edge);
          phc_freq_ppb = (kfit_b - adjfit.slope) * 1e9;
          phc_off_valid = 1;
        }
      }

      /*
       * Sawtooth correction: apply the TIM-TP qErr whose GPS-derived second
       * matches this pulse exactly; a stale or missing message can never
       * smear a wrong pulse. The ±10 ns adjustment can't cross a second
       * boundary in practice.
       */
      double qerr_now = 0;
      int qerr_matched = qerr_lookup(clock_sec, &qerr_now);
      int qerr_applied = 0;
      if (qerr_matched && qerr_sign) {
        long long adj = (long long)(qerr_sign * qerr_now);
        rx_nsec += (long)adj;
        if (rx_nsec < 0) {
          rx_sec--;
          rx_nsec += 1000000000L;
        } else if (rx_nsec >= 1000000000L) {
          rx_sec++;
          rx_nsec -= 1000000000L;
        }
        qerr_applied = 1;
      }

      long offset_ns = rx_nsec;
      if (offset_ns > 500000000L)
        offset_ns -= 1000000000L;

      /* sign experiment: with -q 0, emit every matched pulse */
      if (qerr_sign == 0 && qerr_matched)
        printf("QEXP seq=%u offset=%+ld qerr=%+.1f\n", p->seq, offset_ns,
               qerr_now);

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

      good++;
      if (good <= 10 || (good % 10) == 1) {
        printf("seq=%u offset=%+ld ns A[n=%d rms=%.0fns slope=%.6f] "
               "B[n=%d rms=%.0fns slew=%+.3fppm] "
               "qerr=%+.1f%s [good=%u bad=%u dropped=%u]\n",
               p->seq, offset_ns, iepfit.n, iepfit.rms, iepfit.slope,
               adjfit.n, adjfit.rms, adjfit.slope * 1e6, qerr_now,
               qerr_applied ? " applied" : (qerr_matched ? " logged" : " none"),
               good, bad, dropped);
      }
      if (good <= 10 || (good % 10) == 1) {
        write_prom(offset_ns, iepfit.rms, iepfit.slope, adjfit.rms,
                   adjfit.slope, qerr_now, good, bad, dropped);
      }
    }

    if (!(pfds[0].revents & POLLIN))
      continue; /* idle tick: sampled + drained, nothing else to do */

    /* drain the rpmsg message */
    (void)read(rpmsg_fd, rpmsg_buf, sizeof(rpmsg_buf));

    uint32_t seq = pru->seq;
    if (seq == last_seq)
      continue;
    last_seq = seq;

    /*
     * Place the latched edge on the extended timeline WITHOUT committing it
     * to the unwrapper — the edge predates the sampler read above, so it
     * must not move iep_last_raw backwards.
     */
    int64_t pps_ext = iep_ext + (int32_t)(pru->iep_lo - iep_last_raw);

    if (have_prev) {
      int64_t delta = pps_ext - prev_pps_ext;
      if (delta < 150000000LL || delta > 250000000LL) {
        bad++;
        prev_pps_ext = pps_ext;
        continue;
      }
      t2_gps_ticks += delta; /* per-second timebase truth */
      t2_gps_secs++;
    } else {
      have_prev = 1;
      prev_pps_ext = pps_ext;
      printf("seq=%u pps_ext=%lld (first pulse)\n", seq, (long long)pps_ext);
      continue;
    }
    prev_pps_ext = pps_ext;

    if (pend_count < PEND_MAX) {
      pend[(pend_head + pend_count) % PEND_MAX] =
          (struct pending){pps_ext, seq};
      pend_count++;
    } else {
      dropped++;
    }
  }

  shm->valid = 0;
  close(rpmsg_fd);
  printf("pru_pps_shm: exiting (good=%u bad=%u dropped=%u)\n", good, bad,
         dropped);
  return 0;
}
