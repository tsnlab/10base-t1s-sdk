// SPDX-License-Identifier: GPL-3.0+
/*
 * LAN865x architecture-specific header
 *
 * Copyright (c) 2026 TSN Lab
 * Original Author: Jihoon Park <pakji@tsnlab.com>
 */

#ifndef LAN865X_ARCH_H
#define LAN865X_ARCH_H

#include <linux/cdev.h>
#include <linux/i2c.h>
#include <linux/net_tstamp.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "lan865x_health.h"

#ifdef __LAN865X_DEBUG__
#define LAN865X_DEBUG(...) pr_debug(__VA_ARGS__)
#else
#define LAN865X_DEBUG(...)
#endif // __LAN865X_DEBUG__

/* SDK copy, not <linux/oa_tc6.h>: Linux >= 6.12 ships a different mainline
 * header of that name and kbuild searches the kernel include tree first.
 */
#include "../../../include/linux/oa_tc6.h"

#define LAN865X_BUFFER_SIZE (1560)

#define MMS4_PLCA_STS 0x0004CA03
#define PLCA_BEACON_TX_RX_MASK (1 << 15)

#define MMS1_MAC_TSH 0x00010070
#define MMS1_MAC_TSL 0x00010074
#define MMS1_MAC_TN 0x00010075
#define MMS1_MAC_TA 0x00010076
#define MMS1_MAC_TI 0x00010077
#define MMS1_MAC_TISUBN 0x0001006F
#define TISUBNS_FRAC_BITS 24

#define MMS0_TTSCAH 0x10
#define MMS0_TTSCAL 0x11
#define MMS0_TTSCBH 0x12
#define MMS0_TTSCBL 0x13
#define MMS0_TTSCCH 0x14
#define MMS0_TTSCCL 0x15
#define MMS0_STATS11 0x0213
#define MMS0_STATS12 0x0214

#define MMS0_OA_STATUS0 0x00000008
#define MMS0_OA_STATUS1 0x00000009
#define TS_A_MASK (1 << 8)
#define TS_B_MASK (1 << 9)
#define TS_C_MASK (1 << 10)

#define MMS0_OA_MASK0 0x00000013
#define TS_A_INT_MASK (1 << 8)
#define TS_B_INT_MASK (1 << 9)
#define TS_C_INT_MASK (1 << 10)

enum lan865x_timestamp_id {
    LAN865X_TIMESTAMP_ID_NONE = 0,
    LAN865X_TIMESTAMP_ID_GPTP,     // GPTP
    LAN865X_TIMESTAMP_ID_NORMAL,   // NORMAL
    LAN865X_TIMESTAMP_ID_RESERVED, // RESERVED
    LAN865X_TIMESTAMP_ID_MAX,
};

#define NS_IN_1S 1000000000

/* 25Mhz = LAN8650 SPI MAX Hz */
#define TICKS_SCALE 40
#define RESERVED_CYCLE 25000000

typedef u64 sysclock_t;
typedef u64 timestamp_t;

struct ptp_device {
    struct device* dev;
    struct oa_tc6* tc6;
    struct ptp_clock* ptp_clock;
    struct ptp_clock_info ptp_info;

    struct task_struct* ptp_thread;
    wait_queue_head_t txts_wq;                          /* woken when a TX timestamp is requested */
    spinlock_t txts_lock;                               /* protects lan865x_priv.waiting_txts_skb[] */
    unsigned long txts_start[LAN865X_TIMESTAMP_ID_MAX]; /* jiffies */

    u32 ti_subnano_b24; // timer increase every clock (25MHz) cycle
    u64 offset;

    struct mutex lock;
};

struct lan865x_priv {
    struct work_struct multicast_work;
    struct net_device* netdev;
    struct spi_device* spi;
    struct oa_tc6* tc6;

    struct ptp_device* ptpdev;
    struct hwtstamp_config tstamp_config;
    struct sk_buff* waiting_txts_skb[LAN865X_TIMESTAMP_ID_MAX - 1];

    uint64_t total_tx_count;
    uint64_t total_tx_drop_count;

    struct i2c_client* fxl6408_client;
    struct mutex fxl6408_lock;
    bool fxl6408_initialized;
    u8 node_id;
    u8 chip; /* LAN865X_CHIP_* (t1s_oaspi.h), from the DT match */

    struct task_struct* plca_check_thread;

    /* /dev/lan865xN register access node (LAN865X_READ_REG/WRITE_REG) */
    struct cdev cdev;
    dev_t devt;
    bool cdev_added;

    /* lan865x_health.c */
    struct delayed_work mon_work;
    struct mutex hw_lock; /* monitor work vs. recovery */
    spinlock_t stats_lock;
    u64 hwstats[LAN865X_HWSTAT_NUM];
    /* TX stall detection / recovery */
    struct delayed_work rcv_work;
    unsigned long rcv_flags; /* LAN865X_RCV_PENDING, LAN865X_RCV_STOPPING */
    int rcv_reason;
    unsigned long rcv_last; /* jiffies of the last recovery */
    unsigned int rcv_backoff_ms;
    bool rcv_phy_started;
    unsigned long stall_since; /* jiffies, 0 = no stall suspected */
    u64 stall_tftx;            /* chip TFTX when the suspicion started */
    u64 replay_host_base;      /* tx_packets at the last (re)start */
    u64 replay_chip_base;      /* chip FTX at the last (re)start */
    u64 replay_att_base;       /* chip TFTX at the last (re)start */
    /* collision detect policy */
    int cden_cur; /* -1 = unknown (after reset) */
    int pst_ok_polls;
};

struct lan865x_priv* get_lan865x_priv_by_ptp_info(struct ptp_clock_info* ptp_info);

sysclock_t lan865x_get_sys_clock(struct lan865x_priv* priv);
sysclock_t lan865x_get_sys_clock_and_delay(struct lan865x_priv* priv, struct ptp_system_timestamp* res_spi_delay);
int lan865x_set_sys_clock(struct lan865x_priv* priv, u64 timestamp);
u32 lan865x_get_cycle_1s(void);
int lan865x_set_sys_clock_ti(struct lan865x_priv* priv, u64 subnano_b24);
int lan865x_add_sys_clock(struct lan865x_priv* priv, u32 add_offset);
int lan865x_sub_sys_clock(struct lan865x_priv* priv, u32 sub_offset);
timestamp_t lan865x_read_tx_timestamp(struct lan865x_priv* priv, int tx_id);
u64 lan865x_get_tx_packets(struct lan865x_priv* priv);
u64 lan865x_get_tx_drop_packets(struct lan865x_priv* priv);
int lan865x_update_tx_packets(struct lan865x_priv* priv);
int lan865x_set_nodeid(struct lan865x_priv* priv, u32 node_id);

#endif /* LAN865X_ARCH_H */
