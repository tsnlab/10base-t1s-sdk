// SPDX-License-Identifier: GPL-2.0+
/*
 * LAN865x link health: MAC/PHY statistics, TX stall detection/recovery,
 * collision-detect policy.
 *
 * Register references are to the LAN8650/1 data sheet DS60001734F.
 *
 * Copyright (c) 2026 TSN Lab
 */

#include "lan865x_health.h"
#include "lan865x_arch.h"

#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/workqueue.h>

/* MMS1 STATS0..STATS12 (0x0208..0x0214), all clear-on-read (11.2.26-38).
 * Note: 6.5.2 of the data sheet gives 0x288.. for the block; the register
 * map in 11.2 (used here) gives 0x208..  TODO(spec): confirm on silicon that
 * STATS12.FTX follows tx_packets (docs/notes/32 section 3).
 */
#define LAN865X_MMS1_STATS0 0x00010208
#define LAN865X_NUM_STATS_REGS 13
/* MMS2 T1SPCSDIAG1 (0x08F5) RMTJABCNT, T1SPCSDIAG2 (0x08F6) CORTXCNT (11.3) */
#define LAN865X_MMS2_T1SPCSDIAG1 0x000208F5
/* MMS4 STS1 (0x0018), RC event bits (11.5) */
#define LAN865X_MMS4_STS1 0x00040018

static unsigned int stats_ms = 100;
module_param(stats_ms, uint, 0444);
MODULE_PARM_DESC(stats_ms, "MAC/PHY statistics poll period in ms (default 100, 0 = off)");

static const char lan865x_hwstat_names[LAN865X_HWSTAT_NUM][ETH_GSTRING_LEN] = {
    [LAN865X_HW_RXSE] = "mac_rx_symbol_errors",
    [LAN865X_HW_LFER] = "mac_rx_length_field_errors",
    [LAN865X_HW_OFRX] = "mac_rx_oversize_frames",
    [LAN865X_HW_UFRX] = "mac_rx_undersize_frames",
    [LAN865X_HW_RXRER] = "mac_rx_resource_errors",
    [LAN865X_HW_RXBOVR] = "mac_rx_buffer_overruns",
    [LAN865X_HW_RXFOVR] = "mac_rx_fifo_overruns",
    [LAN865X_HW_FCSE] = "mac_rx_fcs_errors",
    [LAN865X_HW_UHMFRX] = "mac_rx_unicast_hash_frames",
    [LAN865X_HW_MHMFRX] = "mac_rx_multicast_hash_frames",
    [LAN865X_HW_BFRX] = "mac_rx_broadcast_frames",
    [LAN865X_HW_VTRX] = "mac_rx_vlan_frames",
    [LAN865X_HW_TFRX] = "mac_rx_total_frames",
    [LAN865X_HW_FRX] = "mac_rx_frames_ok",
    [LAN865X_HW_TXAIE] = "mac_tx_abort_internal_error",
    [LAN865X_HW_TXAEE] = "mac_tx_abort_external_error",
    [LAN865X_HW_TXFUR] = "mac_tx_fifo_underruns",
    [LAN865X_HW_TXBUR] = "mac_tx_buffer_underruns",
    [LAN865X_HW_XCOL] = "mac_tx_excessive_collisions",
    [LAN865X_HW_TFTX] = "mac_tx_total_frames",
    [LAN865X_HW_FTX] = "mac_tx_frames_ok",
    [LAN865X_HW_RMTJAB] = "phy_rx_remote_jabber",
    [LAN865X_HW_CORTX] = "phy_tx_collisions",
    [LAN865X_HW_STS1_PSTC] = "phy_ev_plca_status_changed",
    [LAN865X_HW_STS1_TXCOL] = "phy_ev_tx_collision",
    [LAN865X_HW_STS1_TXJAB] = "phy_ev_tx_jabber",
    [LAN865X_HW_STS1_EMPCYC] = "phy_ev_plca_empty_cycle",
    [LAN865X_HW_STS1_RXINTO] = "phy_ev_rx_in_own_to",
    [LAN865X_HW_STS1_UNEXPB] = "phy_ev_unexpected_beacon",
    [LAN865X_HW_STS1_BCNBFTO] = "phy_ev_beacon_before_to",
    [LAN865X_HW_STS1_UNCRS] = "phy_ev_unexpected_crs",
    [LAN865X_HW_STS1_PLCASYM] = "phy_ev_plca_symbols",
    [LAN865X_HW_STS1_ESDERR] = "phy_ev_esd_error",
    [LAN865X_HW_STS1_DEC5B] = "phy_ev_5b_decode_error",
    [LAN865X_SW_SATURATED] = "drv_hw_counter_saturated",
    [LAN865X_SW_POLL_ERRORS] = "drv_stats_poll_errors",
};

static void lan865x_add8(struct lan865x_priv* priv, int id, u32 reg, int shift) {
    u32 v = (reg >> shift) & 0xFF;

    priv->hwstats[id] += v;
    if (v == 0xFF) {
        priv->hwstats[LAN865X_SW_SATURATED]++;
    }
}

/* Read and accumulate every clear-on-read counter.  Called with hw_lock. */
static void lan865x_poll_hw_stats(struct lan865x_priv* priv) {
    u32 st[LAN865X_NUM_STATS_REGS];
    u32 diag[2];
    u32 sts1;
    int ret;

    ret = oa_tc6_read_registers(priv->tc6, LAN865X_MMS1_STATS0, st, LAN865X_NUM_STATS_REGS);
    if (!ret) {
        spin_lock_bh(&priv->stats_lock);
        lan865x_add8(priv, LAN865X_HW_RXSE, st[0], 24);
        lan865x_add8(priv, LAN865X_HW_LFER, st[0], 16);
        lan865x_add8(priv, LAN865X_HW_OFRX, st[0], 8);
        lan865x_add8(priv, LAN865X_HW_UFRX, st[0], 0);
        lan865x_add8(priv, LAN865X_HW_RXRER, st[1], 24);
        lan865x_add8(priv, LAN865X_HW_RXBOVR, st[1], 16);
        lan865x_add8(priv, LAN865X_HW_RXFOVR, st[1], 8);
        lan865x_add8(priv, LAN865X_HW_FCSE, st[2], 0);
        /* st[3] STATS3 (type ID match) and st[4] STATS4 (specific address
         * match) are read only because they are in the burst.
         */
        lan865x_add8(priv, LAN865X_HW_UHMFRX, st[5], 24);
        lan865x_add8(priv, LAN865X_HW_MHMFRX, st[5], 16);
        lan865x_add8(priv, LAN865X_HW_BFRX, st[5], 8);
        lan865x_add8(priv, LAN865X_HW_VTRX, st[5], 0);
        priv->hwstats[LAN865X_HW_TFRX] += st[6];
        priv->hwstats[LAN865X_HW_FRX] += st[7];
        lan865x_add8(priv, LAN865X_HW_TXAIE, st[8], 0);
        lan865x_add8(priv, LAN865X_HW_TXAEE, st[9], 24);
        lan865x_add8(priv, LAN865X_HW_TXFUR, st[9], 16);
        lan865x_add8(priv, LAN865X_HW_TXBUR, st[9], 8);
        lan865x_add8(priv, LAN865X_HW_XCOL, st[10], 0);
        priv->hwstats[LAN865X_HW_TFTX] += st[11];
        priv->hwstats[LAN865X_HW_FTX] += st[12];
        spin_unlock_bh(&priv->stats_lock);

        if (st[10] & 0xFF) {
            /* Errata DS80001075F s4: TX may halt after an excessive
             * collision drop.
             */
            net_warn_ratelimited("%s: %u frame(s) dropped after 16 collisions (XCOL)\n", priv->netdev->name,
                                 st[10] & 0xFF);
        }
    } else {
        priv->hwstats[LAN865X_SW_POLL_ERRORS]++;
    }

    ret = oa_tc6_read_registers(priv->tc6, LAN865X_MMS2_T1SPCSDIAG1, diag, 2);
    if (!ret) {
        spin_lock_bh(&priv->stats_lock);
        priv->hwstats[LAN865X_HW_RMTJAB] += diag[0] & 0xFFFF;
        priv->hwstats[LAN865X_HW_CORTX] += diag[1] & 0xFFFF;
        spin_unlock_bh(&priv->stats_lock);
    } else {
        priv->hwstats[LAN865X_SW_POLL_ERRORS]++;
    }

    ret = oa_tc6_read_register(priv->tc6, LAN865X_MMS4_STS1, &sts1);
    if (!ret) {
        static const u8 bit2id[16] = {
            [0] = LAN865X_HW_STS1_DEC5B,   [1] = LAN865X_HW_STS1_ESDERR, [2] = LAN865X_HW_STS1_PLCASYM,
            [3] = LAN865X_HW_STS1_UNCRS,   [4] = LAN865X_HW_STS1_BCNBFTO, [5] = LAN865X_HW_STS1_UNEXPB,
            [6] = LAN865X_HW_STS1_RXINTO,  [7] = LAN865X_HW_STS1_EMPCYC, [9] = LAN865X_HW_STS1_TXJAB,
            [10] = LAN865X_HW_STS1_TXCOL,  [11] = LAN865X_HW_STS1_PSTC,
        };

        spin_lock_bh(&priv->stats_lock);
        for (int b = 0; b < 16; b++) {
            if ((sts1 & BIT(b)) && bit2id[b]) {
                priv->hwstats[bit2id[b]]++;
            }
        }
        spin_unlock_bh(&priv->stats_lock);
    } else {
        priv->hwstats[LAN865X_SW_POLL_ERRORS]++;
    }
}

static void lan865x_mon_work(struct work_struct* work) {
    struct lan865x_priv* priv = container_of(to_delayed_work(work), struct lan865x_priv, mon_work);

    mutex_lock(&priv->hw_lock);
    lan865x_poll_hw_stats(priv);
    mutex_unlock(&priv->hw_lock);

    if (stats_ms) {
        schedule_delayed_work(&priv->mon_work, msecs_to_jiffies(stats_ms));
    }
}

int lan865x_health_init(struct lan865x_priv* priv) {
    mutex_init(&priv->hw_lock);
    spin_lock_init(&priv->stats_lock);
    INIT_DELAYED_WORK(&priv->mon_work, lan865x_mon_work);
    return 0;
}

void lan865x_health_start(struct lan865x_priv* priv) {
    if (stats_ms) {
        schedule_delayed_work(&priv->mon_work, msecs_to_jiffies(stats_ms));
    }
}

void lan865x_health_stop(struct lan865x_priv* priv) {
    cancel_delayed_work_sync(&priv->mon_work);
}

/* netdev statistics = driver software counters + chip MAC/PHY counters */
void lan865x_get_stats64(struct net_device* netdev, struct rtnl_link_stats64* s) {
    struct lan865x_priv* priv = netdev_priv(netdev);
    const u64* h = priv->hwstats;
    u64 rx_err, tx_err;

    netdev_stats_to_stats64(s, &netdev->stats);

    spin_lock_bh(&priv->stats_lock);
    /* Physical collisions seen by the PHY while transmitting (CORTXCNT,
     * DS60001734F 7.2.5).  The MAC itself has no single/multiple/late
     * collision counters, only XCOL.
     */
    s->collisions += h[LAN865X_HW_CORTX];
    s->tx_aborted_errors += h[LAN865X_HW_XCOL];
    s->tx_fifo_errors += h[LAN865X_HW_TXFUR] + h[LAN865X_HW_TXBUR];
    tx_err = h[LAN865X_HW_XCOL] + h[LAN865X_HW_TXFUR] + h[LAN865X_HW_TXBUR] + h[LAN865X_HW_TXAIE] +
             h[LAN865X_HW_TXAEE];
    s->tx_errors += tx_err;

    s->rx_crc_errors += h[LAN865X_HW_FCSE];
    s->rx_frame_errors += h[LAN865X_HW_RXSE];
    s->rx_length_errors += h[LAN865X_HW_LFER] + h[LAN865X_HW_OFRX] + h[LAN865X_HW_UFRX];
    s->rx_over_errors += h[LAN865X_HW_RXBOVR];
    s->rx_fifo_errors += h[LAN865X_HW_RXFOVR];
    s->rx_missed_errors += h[LAN865X_HW_RXRER];
    rx_err = h[LAN865X_HW_FCSE] + h[LAN865X_HW_RXSE] + h[LAN865X_HW_LFER] + h[LAN865X_HW_OFRX] +
             h[LAN865X_HW_UFRX] + h[LAN865X_HW_RXBOVR] + h[LAN865X_HW_RXFOVR] + h[LAN865X_HW_RXRER];
    s->rx_errors += rx_err;
    spin_unlock_bh(&priv->stats_lock);
}

int lan865x_get_sset_count(struct net_device* netdev, int sset) {
    if (sset != ETH_SS_STATS) {
        return -EOPNOTSUPP;
    }
    return LAN865X_HWSTAT_NUM + OA_TC6_STAT_NUM;
}

void lan865x_get_strings(struct net_device* netdev, u32 sset, u8* data) {
    if (sset != ETH_SS_STATS) {
        return;
    }
    for (int i = 0; i < LAN865X_HWSTAT_NUM; i++) {
        memcpy(data, lan865x_hwstat_names[i], ETH_GSTRING_LEN);
        data += ETH_GSTRING_LEN;
    }
    for (int i = 0; i < OA_TC6_STAT_NUM; i++) {
        strscpy(data, oa_tc6_stat_names[i], ETH_GSTRING_LEN);
        data += ETH_GSTRING_LEN;
    }
}

void lan865x_get_ethtool_stats(struct net_device* netdev, struct ethtool_stats* es, u64* data) {
    struct lan865x_priv* priv = netdev_priv(netdev);

    spin_lock_bh(&priv->stats_lock);
    memcpy(data, priv->hwstats, sizeof(u64) * LAN865X_HWSTAT_NUM);
    spin_unlock_bh(&priv->stats_lock);
    oa_tc6_get_stats(priv->tc6, data + LAN865X_HWSTAT_NUM);
}
