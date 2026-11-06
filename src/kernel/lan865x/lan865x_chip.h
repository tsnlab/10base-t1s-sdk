/* SPDX-License-Identifier: GPL-2.0+ */
/* Microchip LAN8650/1 specific part of the OPEN Alliance TC6 driver */
#ifndef LAN865X_CHIP_H
#define LAN865X_CHIP_H

#include <linux/types.h>

struct oa_tc6;
struct oa_tc6_chip_ops;

u8 indirect_read(struct oa_tc6* tc6, u8 addr, u8 mask);
int init_lan865x(struct oa_tc6* tc6);
extern const struct oa_tc6_chip_ops lan865x_chip_ops;

#endif /* LAN865X_CHIP_H */
