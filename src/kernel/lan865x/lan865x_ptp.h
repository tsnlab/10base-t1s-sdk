// SPDX-License-Identifier: GPL-3.0+
/*
 * LAN865x PTP (Precision Time Protocol) header
 *
 * Copyright (c) 2026 TSN Lab
 * Original Author: Jihoon Park <pakji@tsnlab.com>
 */

#ifndef LAN865X_GPTP_H
#define LAN865X_GPTP_H

#ifdef FRAME_TIMESTAMP_ENABLE

#include "lan865x_arch.h"

bool is_gptp_packet(const struct sk_buff* skb);
struct ptp_device* ptp_device_init(struct device* dev, struct oa_tc6* tc6, s32 max_adj);
void ptp_device_destroy(struct ptp_device* ptp);

#endif /* FRAME_TIMESTAMP_ENABLE */

#endif /* LAN865X_GPTP_H */
