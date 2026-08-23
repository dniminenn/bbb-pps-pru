// SPDX-License-Identifier: GPL-2.0
/* cpsw_pruts: hardware NTP timestamps for the built-in cpsw driver.
 * cpsw is CONFIG_TI_CPSW=y on this kernel, so no module swap: hook the
 * live net_device at runtime. rx stamps via rx_handler, tx stamps via a
 * wrapped ndo_start_xmit, both correlated to the PRU1 pktts ring
 * (main_pktts_pru1.c), expressed on the ptp_pruss PHC. rmmod restores
 * the stock pointers; nothing persists across reboot.
 */
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/net_tstamp.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include "ptp_pruss.h"

#define PKTTS_PHYS 0x4A303000
#define ECAP_PHYS 0x4A330000
#define STATS_PHYS 0x4A100900
#define RXGOOD_OFF 0x00
#define TXGOOD_OFF 0x34
#define PK_MAGIC 0x504b5453
#define PK_SEQ 4
#define PK_RING 24
#define RING_N 128
#define EVN 64
#define TICKS_MATCH 400000 /* 2 ms */
#define TICKS_GATE 2000000 /* 10 ms */

static char *ifname = "eth0";
module_param(ifname, charp, 0444);
static char *vlanif = "";
module_param(vlanif, charp, 0444);
MODULE_PARM_DESC(vlanif, "vlan child to rx-hook: vlan_do_receive runs before rx handlers, the parent hook never sees tagged frames");
static unsigned int rx_match, rx_miss;
static unsigned int tx_match, tx_miss, tx_resync;
static unsigned int klat_ns = 60000;
module_param(rx_match, uint, 0444);
module_param(rx_miss, uint, 0444);
module_param(tx_match, uint, 0444);
module_param(tx_miss, uint, 0444);
module_param(tx_resync, uint, 0444);
module_param(klat_ns, uint, 0444);

static struct net_device *ndev, *vdev;
static void __iomem *ring_io, *ecap_io, *stats_io;
static const struct net_device_ops *orig_ndo;
static const struct ethtool_ops *orig_eto;
static struct net_device_ops pruts_ndo;
static struct ethtool_ops pruts_eto;

static DEFINE_SPINLOCK(evlock);
static u32 seen_seq, prev_rxg, prev_txg;
static bool prev_valid;
static u64 rx_ev_t[EVN];
static u32 rx_ev_g[EVN];
static u64 tx_ev_t[EVN];
static u32 tx_ev_g[EVN];
static u32 rx_w, tx_w;
static s64 klat = 12000;

static bool rx_on, tx_on, dying;
static u32 txg_base, xmit_seq;

struct txpend {
	struct sk_buff *skb;
	u64 after;
	u32 want_g;
	unsigned long expire;
};
#define TXQ 8
static struct txpend txq[TXQ];
static u32 txq_r, txq_w;
static struct delayed_work txwork;

static u64 now_ticks(void)
{
	return ptp_pruss_extend(readl(ecap_io));
}

/* evlock held */
static void drain_ring(void)
{
	u32 seq = readl(ring_io + PK_SEQ);
	u32 i = seq - seen_seq > RING_N ? seq - RING_N : seen_seq;

	for (; i != seq; i++) {
		void __iomem *e = ring_io + PK_RING + (i & (RING_N - 1)) * 12;
		u32 rxg, txg;
		u64 t;

		t = ptp_pruss_extend(readl(e));
		rxg = readl(e + 4);
		txg = readl(e + 8);
		if (prev_valid) {
			if (rxg != prev_rxg) {
				rx_ev_t[rx_w & (EVN - 1)] = t;
				rx_ev_g[rx_w & (EVN - 1)] = rxg;
				rx_w++;
			}
			if (txg != prev_txg) {
				tx_ev_t[tx_w & (EVN - 1)] = t;
				tx_ev_g[tx_w & (EVN - 1)] = txg;
				tx_w++;
			}
		}
		prev_rxg = rxg;
		prev_txg = txg;
		prev_valid = true;
	}
	seen_seq = seq;
}

/* evlock held; nearest event wins. Delivered frames always have an
 * event (counted before delivery); RXGOOD also counts wire frames
 * ALE never delivers (~11/s mcast noise), those ghosts only add
 * neighbors, so the error is bounded by the inter-event gap. Exact
 * counting is impossible for rx, wire count != delivered count.
 */
static u64 match_rx(u64 now)
{
	u64 target = now - klat, best = 0;
	s64 bestd = S64_MAX;
	int bi = -1, i;

	for (i = 0; i < EVN; i++) {
		s64 d;

		if (!rx_ev_t[i])
			continue;
		d = (s64)(target - rx_ev_t[i]);
		if (d < 0)
			d = -d;
		if (d < bestd) {
			bestd = d;
			best = rx_ev_t[i];
			bi = i;
		}
	}
	if (bi < 0 || bestd > TICKS_MATCH) {
		rx_miss++;
		return 0;
	}
	rx_ev_t[bi] = 0;
	if ((s64)(now - best) < klat * 4) {	/* spikes don't train klat */
		klat += ((s64)(now - best) - klat) >> 5;
		klat_ns = (u32)(klat * 5);
	}
	rx_match++;
	return best;
}

static rx_handler_result_t pruts_rx(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	unsigned long fl;
	s64 dns = 0;
	u64 now, t;

	if (!rx_on)
		return RX_HANDLER_PASS;
	/* per-frame target: NAPI batches share one now, the
	 * stack-entry stamp separates frames within a batch
	 */
	if (skb->tstamp) {
		dns = ktime_to_ns(skb->tstamp) - ktime_get_real_ns();
		if (dns < -1000000 || dns > 0)
			dns = 0;
	}
	spin_lock_irqsave(&evlock, fl);
	drain_ring();
	now = now_ticks() + div_s64(dns, 5);
	t = match_rx(now);
	if (!t)
		t = now - klat;	/* unbiased estimate beats late kernel stamp */
	spin_unlock_irqrestore(&evlock, fl);
	skb_hwtstamps(skb)->hwtstamp = ns_to_ktime(ptp_pruss_ticks_to_ns(t));
	return RX_HANDLER_PASS;
}

static netdev_tx_t pruts_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct sk_buff *hold = NULL;
	struct sk_buff *drop = NULL;
	netdev_tx_t ret;
	unsigned long fl;
	u64 after = 0;
	u32 seq;

	if (unlikely(tx_on && (skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP))) {
		skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
		hold = skb_get(skb);
		after = now_ticks();
	}
	ret = orig_ndo->ndo_start_xmit(skb, dev);
	if (ret != NETDEV_TX_OK) {
		if (hold)
			kfree_skb(hold);
		return ret;
	}
	/* tx queue lock serializes; count == egress order */
	seq = xmit_seq + 1;
	WRITE_ONCE(xmit_seq, seq);
	if (!hold)
		return ret;
	spin_lock_irqsave(&evlock, fl);
	if (txq_w - txq_r == TXQ) {
		tx_miss++;
		drop = hold;
	} else {
		struct txpend *p = &txq[txq_w++ & (TXQ - 1)];

		p->skb = hold;
		p->after = after;
		p->want_g = txg_base + seq;
		p->expire = jiffies + msecs_to_jiffies(20);
	}
	spin_unlock_irqrestore(&evlock, fl);
	if (drop)
		kfree_skb(drop);
	else
		mod_delayed_work(system_highpri_wq, &txwork, 0);
	return ret;
}

static void txwork_fn(struct work_struct *w)
{
	struct {
		struct sk_buff *skb;
		u64 t;
	} done[TXQ];
	static u32 pend_diff;
	static bool pend_seen;
	unsigned long fl;
	bool pending;
	int nd, i, spins = 0;

again:
	nd = 0;
	spin_lock_irqsave(&evlock, fl);
	drain_ring();
	while (txq_r != txq_w && nd < TXQ) {
		struct txpend *p = &txq[txq_r & (TXQ - 1)];
		u64 t = 0;

		for (i = 0; i < EVN; i++)
			if (tx_ev_t[i] && tx_ev_g[i] == p->want_g) {
				t = tx_ev_t[i];
				break;
			}
		if (t && (s64)(t - p->after) > 0 &&
		    (s64)(t - p->after) < TICKS_GATE) {
			tx_ev_t[i] = 0;
			done[nd].skb = p->skb;
			done[nd++].t = t;
			tx_match++;
			txq_r++;
		} else if (t) {
			/* base wrong; drop, idle resync heals */
			tx_ev_t[i] = 0;
			done[nd].skb = p->skb;
			done[nd++].t = 0;
			tx_miss++;
			txq_r++;
		} else if (time_after(jiffies, p->expire)) {
			done[nd].skb = p->skb;
			done[nd++].t = 0;
			tx_miss++;
			txq_r++;
		} else {
			break;
		}
	}
	/* idle frame-count base resync, two stable reads */
	if (txq_r == txq_w) {
		u32 diff = readl(stats_io + TXGOOD_OFF) -
			   (txg_base + READ_ONCE(xmit_seq));

		if (!diff) {
			pend_seen = false;
		} else if (pend_seen && diff == pend_diff) {
			txg_base += diff;
			tx_resync++;
			pend_seen = false;
		} else {
			pend_diff = diff;
			pend_seen = true;
		}
	}
	pending = txq_r != txq_w;
	spin_unlock_irqrestore(&evlock, fl);

	for (i = 0; i < nd; i++) {
		if (done[i].t) {
			struct skb_shared_hwtstamps h = {
				.hwtstamp = ns_to_ktime(
					ptp_pruss_ticks_to_ns(done[i].t)),
			};
			skb_tstamp_tx(done[i].skb, &h);
		}
		kfree_skb(done[i].skb);
	}
	/* brief in-line wait for egress before jiffy retries */
	if (pending && spins < 8 && !READ_ONCE(dying)) {
		spins++;
		usleep_range(100, 250);
		goto again;
	}
	if (!READ_ONCE(dying))
		queue_delayed_work(system_highpri_wq, &txwork,
				   pending ? 1 : HZ);
}

static int pruts_hwts_set(struct net_device *dev,
			  struct kernel_hwtstamp_config *c,
			  struct netlink_ext_ack *ea)
{
	if (c->tx_type != HWTSTAMP_TX_OFF && c->tx_type != HWTSTAMP_TX_ON)
		return -ERANGE;
	tx_on = c->tx_type == HWTSTAMP_TX_ON;
	rx_on = c->rx_filter != HWTSTAMP_FILTER_NONE;
	if (rx_on)
		c->rx_filter = HWTSTAMP_FILTER_ALL;
	return 0;
}

static int pruts_hwts_get(struct net_device *dev,
			  struct kernel_hwtstamp_config *c)
{
	c->tx_type = tx_on ? HWTSTAMP_TX_ON : HWTSTAMP_TX_OFF;
	c->rx_filter = rx_on ? HWTSTAMP_FILTER_ALL : HWTSTAMP_FILTER_NONE;
	return 0;
}

static int pruts_ts_info(struct net_device *dev, struct ethtool_ts_info *i)
{
	i->so_timestamping = SOF_TIMESTAMPING_TX_HARDWARE |
			     SOF_TIMESTAMPING_RX_HARDWARE |
			     SOF_TIMESTAMPING_RAW_HARDWARE |
			     SOF_TIMESTAMPING_TX_SOFTWARE |
			     SOF_TIMESTAMPING_RX_SOFTWARE |
			     SOF_TIMESTAMPING_SOFTWARE;
	i->phc_index = ptp_pruss_phc_index();
	i->tx_types = BIT(HWTSTAMP_TX_OFF) | BIT(HWTSTAMP_TX_ON);
	i->rx_filters = BIT(HWTSTAMP_FILTER_NONE) | BIT(HWTSTAMP_FILTER_ALL);
	return 0;
}

static int __init pruts_init(void)
{
	int err = -ENOMEM;

	ring_io = ioremap(PKTTS_PHYS, 0x1000);
	ecap_io = ioremap(ECAP_PHYS, 0x100);
	stats_io = ioremap(STATS_PHYS, 0x100);
	if (!ring_io || !ecap_io || !stats_io)
		goto unmap;
	if (readl(ring_io) != PK_MAGIC) {
		pr_err("cpsw_pruts: pktts firmware not live\n");
		err = -ENODEV;
		goto unmap;
	}
	if (ptp_pruss_phc_index() < 0) {
		err = -ENODEV;
		goto unmap;
	}
	ndev = dev_get_by_name(&init_net, ifname);
	if (!ndev) {
		err = -ENODEV;
		goto unmap;
	}
	if (ndev->real_num_tx_queues != 1) {
		err = -EOPNOTSUPP;
		goto putv;
	}
	seen_seq = readl(ring_io + PK_SEQ);
	txg_base = readl(stats_io + TXGOOD_OFF);

	orig_ndo = ndev->netdev_ops;
	orig_eto = ndev->ethtool_ops;
	pruts_ndo = *orig_ndo;
	pruts_ndo.ndo_start_xmit = pruts_xmit;
	pruts_ndo.ndo_hwtstamp_set = pruts_hwts_set;
	pruts_ndo.ndo_hwtstamp_get = pruts_hwts_get;
	pruts_eto = *orig_eto;
	pruts_eto.get_ts_info = pruts_ts_info;

	INIT_DELAYED_WORK(&txwork, txwork_fn);

	if (vlanif[0]) {
		vdev = dev_get_by_name(&init_net, vlanif);
		if (!vdev)
			pr_warn("cpsw_pruts: vlanif %s not found\n", vlanif);
	}

	rtnl_lock();
	err = netdev_rx_handler_register(ndev, pruts_rx, NULL);
	if (err) {
		rtnl_unlock();
		goto putv;
	}
	if (vdev) {
		err = netdev_rx_handler_register(vdev, pruts_rx, NULL);
		if (err) {
			netdev_rx_handler_unregister(ndev);
			rtnl_unlock();
			goto putv;
		}
	}
	ndev->netdev_ops = &pruts_ndo;
	ndev->ethtool_ops = &pruts_eto;
	rtnl_unlock();

	queue_delayed_work(system_highpri_wq, &txwork, HZ);
	pr_info("cpsw_pruts: hooked %s, phc index %d\n", ifname,
		ptp_pruss_phc_index());
	return 0;
putv:
	if (vdev)
		dev_put(vdev);
	dev_put(ndev);
unmap:
	if (ring_io)
		iounmap(ring_io);
	if (ecap_io)
		iounmap(ecap_io);
	if (stats_io)
		iounmap(stats_io);
	return err;
}

static void __exit pruts_exit(void)
{
	rtnl_lock();
	ndev->netdev_ops = orig_ndo;
	ndev->ethtool_ops = orig_eto;
	netdev_rx_handler_unregister(ndev);
	if (vdev)
		netdev_rx_handler_unregister(vdev);
	rtnl_unlock();
	synchronize_net();
	WRITE_ONCE(dying, true);
	cancel_delayed_work_sync(&txwork);
	cancel_delayed_work_sync(&txwork);
	while (txq_r != txq_w)
		kfree_skb(txq[txq_r++ & (TXQ - 1)].skb);
	if (vdev)
		dev_put(vdev);
	dev_put(ndev);
	iounmap(ring_io);
	iounmap(ecap_io);
	iounmap(stats_io);
}

module_init(pruts_init);
module_exit(pruts_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PRU-assisted hw timestamps for cpsw");
