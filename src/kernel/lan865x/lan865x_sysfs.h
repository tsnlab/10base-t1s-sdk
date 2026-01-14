// SPDX-License-Identifier: GPL-2.0+
/*
 * LAN865x sysfs interface header
 */

#ifndef _LAN865X_SYSFS_H_
#define _LAN865X_SYSFS_H_

#include <linux/device.h>

struct lan865x_priv;

/* Function to get node_id - to be implemented in lan865x.c */
int lan865x_get_node_id(struct lan865x_priv* priv);

int lan865x_sysfs_init(void);
void lan865x_sysfs_exit(void);
int lan865x_sysfs_create_device(struct lan865x_priv* priv);
void lan865x_sysfs_remove_device(struct lan865x_priv* priv);

#endif /* _LAN865X_SYSFS_H_ */
