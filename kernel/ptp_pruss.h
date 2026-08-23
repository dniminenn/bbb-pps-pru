/* SPDX-License-Identifier: GPL-2.0 */
#ifndef PTP_PRUSS_H
#define PTP_PRUSS_H
#include <linux/types.h>

u64 ptp_pruss_extend(u32 raw);
u64 ptp_pruss_ticks_to_ns(u64 ticks);
int ptp_pruss_phc_index(void);

#endif
