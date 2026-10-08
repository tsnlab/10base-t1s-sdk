// SPDX-License-Identifier: GPL-3.0+
/*
 * LAN865x PTP (Precision Time Protocol) implementation
 *
 * Copyright (c) 2026 TSN Lab
 * Original Author: Jihoon Park <pakji@tsnlab.com>
 */

#ifdef FRAME_TIMESTAMP_ENABLE

#include "lan865x_ptp.h"

#include <linux/delay.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/mutex.h>

#define NSEC_PER_MHZ 1000
#define MHZ_TO_NS(mhz) (NSEC_PER_MHZ / (mhz))

/* The old thread busy-waited udelay(10) and read OA_STATUS0 over SPI on
 * every iteration, whether or not a timestamp was outstanding: on the lab
 * RPi4 that was one CPU at 100 % ("lan865x-ptp-thread" R, 99.9 %) and
 * ~63,000 SPI control transactions/s on an idle link.  It also wrote the
 * whole STATUS0 back (W1C) whenever a TTSCAx bit was set, clearing error
 * bits that belong to the oa_tc6 extended-status handler, and passed a
 * possibly NULL / already freed waiting_txts_skb to skb_tstamp_tx().
 *
 * Now the thread sleeps until a frame with a TX timestamp request is
 * queued, then polls OA_STATUS0/OA_STATUS1 every ptp_poll_us (sleeping)
 * until the capture is available (TTSCAx, DS60001734F 11.1.6), reported
 * missed (TTSCMx, 11.1.7) or ptp_ts_timeout_ms passed, and clears only the
 * bits it handled.
 */
static unsigned int ptp_poll_us = 100;
module_param(ptp_poll_us, uint, 0644);
MODULE_PARM_DESC(ptp_poll_us, "TX timestamp poll interval in us while a timestamp is outstanding (default 100)");

static unsigned int ptp_ts_timeout_ms = 200;
module_param(ptp_ts_timeout_ms, uint, 0644);
MODULE_PARM_DESC(ptp_ts_timeout_ms, "Give up on a TX timestamp after this many ms (default 200)");

#define TS_MISSED_A_MASK BIT(24) /* OA_STATUS1.TTSCMA */
#define TS_MISSED_B_MASK BIT(25) /* OA_STATUS1.TTSCMB */
#define TS_MISSED_C_MASK BIT(26) /* OA_STATUS1.TTSCMC */

struct lan865x_priv* get_lan865x_priv_by_ptp_info(struct ptp_clock_info* ptp_info) {
    struct ptp_device* ptpdev = container_of(ptp_info, struct ptp_device, ptp_info);
    struct lan865x_priv* priv = dev_get_drvdata(ptpdev->dev);

    return priv;
}

static struct sk_buff* lan865x_take_txts_skb(struct lan865x_priv* priv, int id) {
    struct ptp_device* ptpdev = priv->ptpdev;
    struct sk_buff* skb;

    spin_lock_bh(&ptpdev->txts_lock);
    skb = priv->waiting_txts_skb[id];
    priv->waiting_txts_skb[id] = NULL;
    spin_unlock_bh(&ptpdev->txts_lock);

    return skb;
}

static bool lan865x_txts_pending(struct lan865x_priv* priv) {
    return READ_ONCE(priv->waiting_txts_skb[LAN865X_TIMESTAMP_ID_GPTP]) ||
           READ_ONCE(priv->waiting_txts_skb[LAN865X_TIMESTAMP_ID_NORMAL]);
}

/* Called from ndo_start_xmit (BH): remember a clone of @skb for the TX
 * timestamp and return the capture register to request in the TX data
 * header (TSC field).  skb_clone_sk() holds a socket reference, so the
 * timestamp can be delivered with skb_complete_tx_timestamp() even if the
 * original skb is long gone.
 */
u8 lan865x_ptp_request_tx_ts(struct lan865x_priv* priv, struct sk_buff* skb) {
    struct ptp_device* ptpdev = priv->ptpdev;
    struct sk_buff *clone, *old;
    int id;

    if (!ptpdev) {
        return LAN865X_TIMESTAMP_ID_NONE;
    }

    clone = skb_clone_sk(skb);
    if (!clone) {
        return LAN865X_TIMESTAMP_ID_NONE;
    }

    id = is_gptp_packet(skb) ? LAN865X_TIMESTAMP_ID_GPTP : LAN865X_TIMESTAMP_ID_NORMAL;

    spin_lock_bh(&ptpdev->txts_lock);
    old = priv->waiting_txts_skb[id];
    priv->waiting_txts_skb[id] = clone;
    ptpdev->txts_start[id] = jiffies;
    spin_unlock_bh(&ptpdev->txts_lock);

    if (old) {
        priv->hwstats[LAN865X_SW_PTP_TS_REPLACED]++;
        dev_kfree_skb_any(old);
    }

    skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
    wake_up(&ptpdev->txts_wq);

    return id;
}

static void lan865x_complete_txts(struct lan865x_priv* priv, int id) {
    struct skb_shared_hwtstamps skb_hwts = { 0 };
    struct sk_buff* skb;
    timestamp_t tx_ts;

    tx_ts = lan865x_read_tx_timestamp(priv, id);
    LAN865X_DEBUG("%s: %c Timestamp = %llu.%llu\n", __func__, 'A' + id - 1, tx_ts / NS_IN_1S, tx_ts % NS_IN_1S);

    skb = lan865x_take_txts_skb(priv, id);
    if (!skb) {
        return; /* timed out or replaced meanwhile */
    }

    skb_hwts.hwtstamp = ns_to_ktime(tx_ts);
    skb_complete_tx_timestamp(skb, &skb_hwts);
    priv->hwstats[LAN865X_SW_PTP_TS_OK]++;
}

static void lan865x_drop_txts(struct lan865x_priv* priv, int id, int stat) {
    struct sk_buff* skb = lan865x_take_txts_skb(priv, id);

    if (skb) {
        priv->hwstats[stat]++;
        kfree_skb(skb);
    }
}

static int lan865x_ptp_thread_handler(void* data) {
    struct ptp_device* ptpdev = (struct ptp_device*)data;
    struct lan865x_priv* priv = dev_get_drvdata(ptpdev->dev);
    static const struct {
        int id;
        u32 avail; /* OA_STATUS0 */
        u32 missed; /* OA_STATUS1 */
    } slot[] = {
        { LAN865X_TIMESTAMP_ID_GPTP, TS_A_MASK, TS_MISSED_A_MASK },
        { LAN865X_TIMESTAMP_ID_NORMAL, TS_B_MASK, TS_MISSED_B_MASK },
    };
    u32 st[2]; /* OA_STATUS0, OA_STATUS1 */
    u32 clr0, clr1;

    while (!kthread_should_stop()) {
        if (!lan865x_txts_pending(priv)) {
            wait_event_interruptible_timeout(ptpdev->txts_wq, lan865x_txts_pending(priv) || kthread_should_stop(),
                                             HZ);
            continue;
        }

        /* An SPI error (e.g. during a MAC-PHY reset by the TX stall
         * recovery) is not a reason to end the thread any more.
         */
        if (oa_tc6_read_registers(ptpdev->tc6, MMS0_OA_STATUS0, st, 2)) {
            usleep_range(1000, 2000);
            st[0] = st[1] = 0;
        }

        clr0 = 0;
        clr1 = 0;
        for (int i = 0; i < ARRAY_SIZE(slot); i++) {
            if (st[0] & slot[i].avail) {
                lan865x_complete_txts(priv, slot[i].id);
                clr0 |= slot[i].avail;
            } else if (st[1] & slot[i].missed) {
                lan865x_drop_txts(priv, slot[i].id, LAN865X_SW_PTP_TS_MISSED);
                clr1 |= slot[i].missed;
            } else if (READ_ONCE(priv->waiting_txts_skb[slot[i].id]) &&
                       time_after(jiffies, ptpdev->txts_start[slot[i].id] + msecs_to_jiffies(ptp_ts_timeout_ms))) {
                lan865x_drop_txts(priv, slot[i].id, LAN865X_SW_PTP_TS_TIMEOUT);
            }
        }
        // RESERVED (capture C is never requested by this driver)
        if (st[0] & TS_C_MASK) {
            lan865x_read_tx_timestamp(priv, LAN865X_TIMESTAMP_ID_RESERVED);
            clr0 |= TS_C_MASK;
        }
        if (st[1] & TS_MISSED_C_MASK) {
            clr1 |= TS_MISSED_C_MASK;
        }

        /* W1C only what was handled here */
        if (clr0) {
            oa_tc6_write_register(ptpdev->tc6, MMS0_OA_STATUS0, clr0);
        }
        if (clr1) {
            oa_tc6_write_register(ptpdev->tc6, MMS0_OA_STATUS1, clr1);
        }

        if (lan865x_txts_pending(priv)) {
            usleep_range(ptp_poll_us, ptp_poll_us + ptp_poll_us / 2 + 1);
        }
    }

    return 0;
}

bool is_gptp_packet(const struct sk_buff* skb) {
    struct ethhdr* eth;
    struct vlan_hdr* vlan;
    __be16 proto;

    eth = (struct ethhdr*)skb->data;
    proto = ntohs(eth->h_proto);

    if (proto == ETH_P_8021Q || proto == ETH_P_8021AD) {
        vlan = (struct vlan_hdr*)(skb->data + sizeof(struct ethhdr));

        return ntohs(vlan->h_vlan_encapsulated_proto) == ETH_P_1588;
    }

    return proto == ETH_P_1588;
}

static int lan865x_ptp_adjfine(struct ptp_clock_info* ptp_info, long scaled_ppm) {
    u64 ticks_scale, diff_b24;
    u32 ppm;
    int is_negative = 0;

    struct lan865x_priv* priv = get_lan865x_priv_by_ptp_info(ptp_info);
    struct ptp_device* ptpdev = priv->ptpdev;

    LAN865X_DEBUG("lan865x: call %s", __func__);

    mutex_lock(&ptpdev->lock);

    if (scaled_ppm == 0) {
        goto exit;
    }

    if (scaled_ppm < 0) {
        is_negative = 1;
        scaled_ppm = -scaled_ppm;
    }
    ppm = scaled_ppm >> 16;

    /* Adjust ticks_scale */
    diff_b24 = mul_u64_u64_div_u64(TICKS_SCALE << (24 - 16), (u64)scaled_ppm, 1000000ULL);
    ticks_scale = ((TICKS_SCALE << 24) + (is_negative ? -diff_b24 : diff_b24));

    lan865x_set_sys_clock_ti(priv, ticks_scale);
    ptpdev->ti_subnano_b24 = ticks_scale;

    LAN865X_DEBUG("%s: scaled_ppm = %ld, diff = %llu, ticks_scale = %llu = %014llx\n", __func__, scaled_ppm, diff_b24,
                  ticks_scale, ticks_scale);

exit:
    mutex_unlock(&ptpdev->lock);

    return 0;
}

static int lan865x_ptp_adjtime(struct ptp_clock_info* ptp_info, s64 delta_ns) {
    struct lan865x_priv* priv = get_lan865x_priv_by_ptp_info(ptp_info);
    struct ptp_device* ptpdev = priv->ptpdev;

    bool is_negative = false;
    timestamp_t hw_timestamp = 0;
    timestamp_t curr_hw_timestamp = 0;

    LAN865X_DEBUG("lan865x: call %s\n", __func__);

    mutex_lock(&ptpdev->lock);

    if (delta_ns == 0) {
        goto exit;
    }

    hw_timestamp = lan865x_get_sys_clock(priv);

    if (delta_ns < 0) {
        is_negative = true;
        delta_ns = abs(delta_ns);
    }

    hw_timestamp += is_negative ? -delta_ns : delta_ns;

    lan865x_set_sys_clock(priv, hw_timestamp);
    curr_hw_timestamp = lan865x_get_sys_clock(priv);

    LAN865X_DEBUG("%s: delta_ns = %c%llu, curr_hw_timestamp = %llu\n", __func__, is_negative ? '-' : '+', delta_ns,
                  curr_hw_timestamp);

exit:
    mutex_unlock(&ptpdev->lock);

    return 0;
}

static int lan865x_ptp_gettimex64(struct ptp_clock_info* ptp_info, struct timespec64* res_ts,
                                  struct ptp_system_timestamp* sts) {
    u64 timestamp;

    LAN865X_DEBUG("lan865x: call %s", __func__);

    struct lan865x_priv* priv = get_lan865x_priv_by_ptp_info(ptp_info);
    struct ptp_device* ptpdev = priv->ptpdev;

    mutex_lock(&ptpdev->lock);
    timestamp = lan865x_get_sys_clock_and_delay(priv, sts);
    mutex_unlock(&ptpdev->lock);

    *res_ts = ns_to_timespec64(timestamp);

   /*
    * NOTE:
    * Since the LAN865x device communicates over SPI, consecutive register accesses
    * without sufficient delay can cause SPI buffering and transaction backlog.
    * This may introduce additional latency, resulting in a large time offset
    * between the host clock and the device clock (e.g., observed via phc2sys).
    *
    * Therefore, appropriate inter-access delays are required to ensure
    * stable SPI transactions and accurate time synchronization.
    */
    udelay(500);

    LAN865X_DEBUG("%s: timestamp = %llu, res_ts = %llu.%llu\n", __func__, timestamp, res_ts->tv_sec, res_ts->tv_nsec);

    return 0;
}

static int lan865x_ptp_settime64(struct ptp_clock_info* ptp_info, const struct timespec64* set_ts) {
    (void)set_ts;
    u64 host_timestamp;

    struct lan865x_priv* priv = get_lan865x_priv_by_ptp_info(ptp_info);
    struct ptp_device* ptpdev = priv->ptpdev;

    LAN865X_DEBUG("lan865x: call %s", __func__);

    mutex_lock(&ptpdev->lock);

    /* Get host timestamp */
    host_timestamp = (u64)set_ts->tv_sec * NS_IN_1S + set_ts->tv_nsec;

    // TODO add/sub
    lan865x_set_sys_clock(priv, host_timestamp);

    mutex_unlock(&ptpdev->lock);

    return 0;
}

struct ptp_device* ptp_device_init(struct device* dev, struct oa_tc6* tc6, s32 max_adj) {
    struct ptp_device* ptpdev;

    ptpdev = kzalloc(sizeof(struct ptp_device), GFP_KERNEL);
    if (!ptpdev) {
        dev_err(dev, "kzalloc()");
        return NULL;
    }

    struct ptp_clock_info ptp_info = {
        .owner = THIS_MODULE,
        .name = "ptp",
        .max_adj = max_adj,
        .n_ext_ts = 0,
        .pps = 0,
        .adjfine = lan865x_ptp_adjfine,
        .adjtime = lan865x_ptp_adjtime,
        .gettimex64 = lan865x_ptp_gettimex64,
        .settime64 = lan865x_ptp_settime64,
    };

    mutex_init(&ptpdev->lock);
    init_waitqueue_head(&ptpdev->txts_wq);
    spin_lock_init(&ptpdev->txts_lock);

    ptpdev->dev = dev;
    ptpdev->tc6 = tc6;

    /* Fill ptp_info before registering: ptp_clock_register() keeps the
     * pointer, and the old order exposed /dev/ptpN with all-NULL ops until
     * the assignment after it ran.
     */
    ptpdev->ptp_info = ptp_info;

    ptpdev->ptp_clock = ptp_clock_register(&ptpdev->ptp_info, dev);
    if (IS_ERR(ptpdev->ptp_clock)) {
        dev_err(dev, "Failed to register ptp clock\n");
        kfree(ptpdev);
        return NULL;
    }

    // TODO: read from register
    ptpdev->ti_subnano_b24 = TICKS_SCALE << TISUBNS_FRAC_BITS;

    ptpdev->ptp_thread = kthread_run(lan865x_ptp_thread_handler, ptpdev, "lan865x-ptp-thread");
    if (IS_ERR(ptpdev->ptp_thread)) {
        dev_err(ptpdev->dev, "Failed to create PTP thread\n");
        kfree(ptpdev);
        return NULL;
    }

    /* TODO: Configure the OA_MASK0 register to generate an interrupt on Tx Timestamp Capture.
    u32 regval;
    regval = 0;
    oa_tc6_read_register(ptpdev->tc6, 0x0000000C, &regval); // 0x0000_000C = MMS0_OA_MASK0
    LAN865X_DEBUG("%s: OA_MASK0 = 0x%08X\n", __func__, regval);
    regval &= ~(TS_A_INT_ENABLE | TS_B_INT_ENABLE | TS_C_INT_ENABLE);
    oa_tc6_write_register(ptpdev->tc6, 0x0000000C, &regval); // 0x0000_000C = MMS0_OA_MASK0
    regval = 0;
    oa_tc6_read_register(ptpdev->tc6, 0x0000000C, &regval); // 0x0000_000C = MMS0_OA_MASK0
    LAN865X_DEBUG("%s: OA_MASK0 = 0x%08X\n", __func__, regval);
    */

    return ptpdev;
}

void ptp_device_destroy(struct ptp_device* ptp) {
    if (!ptp) {
        return;
    }

    if (ptp->ptp_thread && !IS_ERR(ptp->ptp_thread)) {
        kthread_stop(ptp->ptp_thread);
    }

    {
        struct lan865x_priv* priv = dev_get_drvdata(ptp->dev);

        if (priv) {
            for (int id = LAN865X_TIMESTAMP_ID_GPTP; id <= LAN865X_TIMESTAMP_ID_NORMAL; id++) {
                kfree_skb(lan865x_take_txts_skb(priv, id));
            }
        }
    }

    if (ptp->ptp_clock && !IS_ERR(ptp->ptp_clock)) {
        ptp_clock_unregister(ptp->ptp_clock);
    }

    kfree(ptp);
}

#endif /* FRAME_TIMESTAMP_ENABLE */
