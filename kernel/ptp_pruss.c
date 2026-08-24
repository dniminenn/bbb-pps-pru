// SPDX-License-Identifier: GPL-2.0
/* ptp_pruss: ICSS eCAP TSCTR exposed as a PHC, free-running in rate.
 * PHC time = 64-bit extended tick count * 5 ns + a settable phase base
 * (settime/adjtime, so the timescale can be placed at TAI once at boot).
 * Frequency is deliberately not adjustable; consumers relate the clock to
 * system time themselves (chrony hwtimestamp, ptp4l), like any NIC PHC.
 * The base flows through ticks_to_ns so packet stamps and the PHC stay
 * one timescale.
 */
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define ECAP_PHYS 0x4A330000
#define TICK_NS 5

static void __iomem *ecap;
static struct ptp_clock *clk;
static spinlock_t lock;
static u64 ext_ticks;
static s64 base_ns;
static u32 last_raw;
static struct delayed_work unwrap_work;

static u64 pruss_ticks_now(void)
{
	unsigned long fl;
	u32 raw;
	u64 t;

	spin_lock_irqsave(&lock, fl);
	raw = readl(ecap);
	ext_ticks += (s32)(raw - last_raw);
	last_raw = raw;
	t = ext_ticks;
	spin_unlock_irqrestore(&lock, fl);
	return t;
}

static u64 pruss_ns_now(void)
{
	unsigned long fl;
	u32 raw;
	u64 ns;

	spin_lock_irqsave(&lock, fl);
	raw = readl(ecap);
	ext_ticks += (s32)(raw - last_raw);
	last_raw = raw;
	ns = ext_ticks * TICK_NS + base_ns;
	spin_unlock_irqrestore(&lock, fl);
	return ns;
}

/* place a foreign raw capture on the extended timeline */
u64 ptp_pruss_extend(u32 raw)
{
	unsigned long fl;
	u64 t;

	spin_lock_irqsave(&lock, fl);
	t = ext_ticks + (s32)(raw - last_raw);
	spin_unlock_irqrestore(&lock, fl);
	return t;
}
EXPORT_SYMBOL_GPL(ptp_pruss_extend);

u64 ptp_pruss_ticks_to_ns(u64 ticks)
{
	unsigned long fl;
	s64 b;

	spin_lock_irqsave(&lock, fl);
	b = base_ns;
	spin_unlock_irqrestore(&lock, fl);
	return ticks * TICK_NS + b;
}
EXPORT_SYMBOL_GPL(ptp_pruss_ticks_to_ns);

int ptp_pruss_phc_index(void)
{
	return clk ? ptp_clock_index(clk) : -1;
}
EXPORT_SYMBOL_GPL(ptp_pruss_phc_index);

static void unwrap_fn(struct work_struct *w)
{
	pruss_ticks_now(); /* keep extension alive */
	schedule_delayed_work(&unwrap_work, HZ * 5);
}

static int pp_gettimex(struct ptp_clock_info *i, struct timespec64 *ts,
		       struct ptp_system_timestamp *sts)
{
	u64 ns;

	ptp_read_system_prets(sts);
	ns = pruss_ns_now();
	ptp_read_system_postts(sts);
	*ts = ns_to_timespec64(ns);
	return 0;
}

static int pp_settime(struct ptp_clock_info *i, const struct timespec64 *ts)
{
	unsigned long fl;
	u32 raw;

	spin_lock_irqsave(&lock, fl);
	raw = readl(ecap);
	ext_ticks += (s32)(raw - last_raw);
	last_raw = raw;
	base_ns = timespec64_to_ns(ts) - (s64)(ext_ticks * TICK_NS);
	spin_unlock_irqrestore(&lock, fl);
	return 0;
}
static int pp_nope_adjfine(struct ptp_clock_info *i, long ppm)
{
	return -EOPNOTSUPP;
}
static int pp_adjtime(struct ptp_clock_info *i, s64 d)
{
	unsigned long fl;

	spin_lock_irqsave(&lock, fl);
	base_ns += d;
	spin_unlock_irqrestore(&lock, fl);
	return 0;
}

static struct ptp_clock_info pp_info = {
	.owner = THIS_MODULE,
	.name = "pruss-ecap",
	.max_adj = 0,
	.gettimex64 = pp_gettimex,
	.settime64 = pp_settime,
	.adjfine = pp_nope_adjfine,
	.adjtime = pp_adjtime,
};

static int __init pp_init(void)
{
	ecap = ioremap(ECAP_PHYS, 0x100);
	if (!ecap)
		return -ENOMEM;
	spin_lock_init(&lock);
	last_raw = readl(ecap);
	clk = ptp_clock_register(&pp_info, NULL);
	if (IS_ERR(clk)) {
		iounmap(ecap);
		return PTR_ERR(clk);
	}
	INIT_DELAYED_WORK(&unwrap_work, unwrap_fn);
	schedule_delayed_work(&unwrap_work, HZ * 5);
	pr_info("ptp_pruss: PHC index %d\n", ptp_clock_index(clk));
	return 0;
}

static void __exit pp_exit(void)
{
	cancel_delayed_work_sync(&unwrap_work);
	ptp_clock_unregister(clk);
	clk = NULL;
	iounmap(ecap);
}

module_init(pp_init);
module_exit(pp_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ICSS eCAP timebase as a PHC");
