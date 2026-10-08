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

#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/phy.h>
#include <linux/rtnetlink.h>
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
    [LAN865X_SW_STALL_DETECTED] = "drv_tx_stall_detected",
    [LAN865X_SW_REPLAY_DETECTED] = "drv_tx_replay_detected",
    [LAN865X_SW_WATCHDOG] = "drv_tx_watchdog",
    [LAN865X_SW_FATAL] = "drv_fatal_errors",
    [LAN865X_SW_KICKS] = "drv_tx_kicks",
    [LAN865X_SW_RCV_LIGHT_OK] = "drv_recovery_txen_ok",
    [LAN865X_SW_RCV_RESET_OK] = "drv_recovery_reset_ok",
    [LAN865X_SW_RCV_FAILED] = "drv_recovery_failed",
    [LAN865X_SW_PTP_TS_OK] = "ptp_tx_timestamps",
    [LAN865X_SW_PTP_TS_MISSED] = "ptp_tx_ts_missed",
    [LAN865X_SW_PTP_TS_TIMEOUT] = "ptp_tx_ts_timeouts",
    [LAN865X_SW_PTP_TS_REPLACED] = "ptp_tx_ts_replaced",
    [LAN865X_SW_CDEN_CHANGES] = "drv_cden_changes",
    [LAN865X_SW_CDEN] = "drv_cden",
    [LAN865X_SW_PLCA_PST] = "drv_plca_pst",
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

/* ------------------------------------------------------------------------
 * TX stall detection and recovery
 *
 * docs/notes/32: twice the LAN8651 stopped transmitting for good (RX kept
 * working) after excessive collisions; errata DS80001075F s4 "Packet
 * transmission halts on occurrence of excessive collisions".  In this
 * driver "RX works but TX never resumes" can only mean that every RX footer
 * reports TXC = 0.  Before this change nothing noticed it (no
 * ndo_tx_timeout, no MAC statistics) and only a reboot recovered.
 *
 * Detection (any of):
 *  - stall:   a TX skb is pending, the footer TXC is 0, OA_BUFSTS.TXC reads
 *             0 and the chip's TFTX ("frames attempted, incl. errors") has
 *             not moved for tx_stall_ms.  A merely congested segment keeps
 *             TFTX moving (every frame ends either sent or as XCOL).
 *  - replay:  the chip's FTX (frames sent OK) since the last (re)start
 *             exceeds the frames the host handed over by more than
 *             replay_slack.  First stall: the chip re-sent 25 old frames
 *             ~8000 times/s without any SPI TX.
 *  - watchdog: ndo_tx_timeout (queue stopped for watchdog_timeo).
 *  - fatal:   oa_tc6 fatal error (TXNER/RXNER/FSMSTER/BUSER, unexpected
 *             RESETC, TXPE/LOFE/HDRE, footer errors, SPI errors).
 *
 * Recovery (recovery_mode):
 *  0  log only (and dump state)
 *  1  park the data path, toggle MAC_NCR.TXEN ("writing 0 stops
 *     transmission immediately, the transmit pipeline is cleared",
 *     DS60001734F 11.2.1) and check OA_BUFSTS.TXC; if the credits did not
 *     come back, fall through to 2.  Experimental: whether the TXEN toggle
 *     releases the TX buffer is exactly what is not known yet.
 *  2  park the data path, software-reset the MAC-PHY, redo the full
 *     configuration (oa_tc6_hw_reinit() + lan865x_restore_hw_config()),
 *     restart.  Default.
 * Recoveries closer than 10 s to the previous one are delayed with an
 * exponential back-off (1 s .. 30 s).  The PTP hardware clock restarts
 * from 0 after a reset.
 */
static unsigned int tx_stall_ms = 2000;
module_param(tx_stall_ms, uint, 0644);
MODULE_PARM_DESC(tx_stall_ms, "TX stall detection time in ms (also the netdev watchdog timeout), default 2000");

static unsigned int recovery_mode = 2;
module_param(recovery_mode, uint, 0644);
MODULE_PARM_DESC(recovery_mode, "TX stall/fatal error recovery: 0 = log only, 1 = TXEN toggle then reset, 2 = reset (default)");

static unsigned int replay_slack = 64;
module_param(replay_slack, uint, 0644);
MODULE_PARM_DESC(replay_slack, "Replay detection: chip FTX may exceed host TX frames by this much (0 = off), default 64");

#define LAN865X_RCV_PENDING 0
#define LAN865X_RCV_STOPPING 1
#define LAN865X_RCV_PARKED 2 /* oa_tc6_stop_traffic() done, start_traffic() not yet */

enum lan865x_rcv_reason {
    LAN865X_RCV_STALL,
    LAN865X_RCV_REPLAY,
    LAN865X_RCV_WATCHDOG,
    LAN865X_RCV_FATAL,
};

static const char* const lan865x_rcv_reason_str[] = {
    [LAN865X_RCV_STALL] = "TX stall (TXC=0, no TX progress)",
    [LAN865X_RCV_REPLAY] = "TX replay (chip sent frames the host never gave it)",
    [LAN865X_RCV_WATCHDOG] = "netdev TX watchdog",
    [LAN865X_RCV_FATAL] = "fatal MAC-PHY error",
};

#define LAN865X_MMS1_MAC_NCR 0x00010000
#define LAN865X_MAC_NCR_TXEN BIT(3)
#define LAN865X_MMS4_PLCA_STS 0x0004CA03
#define LAN865X_MMS4_CDCTL0 0x00040087
#define LAN865X_TXC_MAX 0x30 /* OA_BUFSTS.TXC reset value: 48 chunks */

static void lan865x_trigger_recovery(struct lan865x_priv* priv, int reason) {
    unsigned int delay = 0;

    if (test_bit(LAN865X_RCV_STOPPING, &priv->rcv_flags)) {
        return;
    }
    if (test_and_set_bit(LAN865X_RCV_PENDING, &priv->rcv_flags)) {
        return;
    }

    priv->rcv_reason = reason;
    if (priv->rcv_last && time_before(jiffies, priv->rcv_last + 10 * HZ)) {
        priv->rcv_backoff_ms = clamp(priv->rcv_backoff_ms * 2, 1000U, 30000U);
        delay = priv->rcv_backoff_ms;
    } else {
        priv->rcv_backoff_ms = 0;
    }
    netdev_warn(priv->netdev, "%s detected, recovery in %u ms (mode %u)\n", lan865x_rcv_reason_str[reason], delay,
                recovery_mode);
    schedule_delayed_work(&priv->rcv_work, msecs_to_jiffies(delay));
}

static void lan865x_fatal_cb(void* ctx) {
    struct lan865x_priv* priv = ctx;

    priv->hwstats[LAN865X_SW_FATAL]++;
    lan865x_trigger_recovery(priv, LAN865X_RCV_FATAL);
}

void lan865x_health_tx_timeout(struct net_device* netdev, unsigned int txqueue) {
    struct lan865x_priv* priv = netdev_priv(netdev);

    priv->hwstats[LAN865X_SW_WATCHDOG]++;
    lan865x_trigger_recovery(priv, LAN865X_RCV_WATCHDOG);
}

static void lan865x_rebaseline(struct lan865x_priv* priv) {
    priv->replay_host_base = priv->netdev->stats.tx_packets;
    priv->replay_chip_base = priv->hwstats[LAN865X_HW_FTX];
    priv->replay_att_base = priv->hwstats[LAN865X_HW_TFTX];
    priv->stall_since = 0;
    priv->cden_cur = -1; /* init_lan865x() set CDEN = 1; re-apply the policy */
    priv->pst_ok_polls = 0;
}

/* Called with hw_lock, right after a statistics poll. */
static void lan865x_check_tx(struct lan865x_priv* priv) {
    struct oa_tc6_tx_state st;
    u64 host, chip, att;
    u8 txc, rba;

    if (test_bit(LAN865X_RCV_PENDING, &priv->rcv_flags)) {
        return;
    }

    oa_tc6_get_tx_state(priv->tc6, &st);
    if (st.disabled) {
        return; /* fatal path already triggered the recovery */
    }

    host = priv->netdev->stats.tx_packets - priv->replay_host_base;
    chip = priv->hwstats[LAN865X_HW_FTX] - priv->replay_chip_base;
    att = priv->hwstats[LAN865X_HW_TFTX] - priv->replay_att_base;
    /* FTX (sent OK) can never exceed what the host handed over.  TFTX
     * ("attempted, incl. errors") might count each of up to 16 attempts
     * (TODO(spec): per frame or per attempt is not stated), hence x16.
     */
    if (replay_slack && (chip > host + replay_slack || att > 16 * (host + replay_slack))) {
        netdev_err(priv->netdev, "chip transmitted %llu frames OK / %llu attempted, host handed over %llu\n", chip,
                   att, host);
        priv->hwstats[LAN865X_SW_REPLAY_DETECTED]++;
        lan865x_trigger_recovery(priv, LAN865X_RCV_REPLAY);
        return;
    }

    if (!st.tx_pending || st.tx_credits) {
        priv->stall_since = 0;
        return;
    }

    if (!priv->stall_since || priv->hwstats[LAN865X_HW_TFTX] != priv->stall_tftx) {
        /* first suspicion, or the chip is still finishing frames */
        priv->stall_since = jiffies | 1;
        priv->stall_tftx = priv->hwstats[LAN865X_HW_TFTX];
        return;
    }

    if (time_before(jiffies, priv->stall_since + msecs_to_jiffies(tx_stall_ms))) {
        return;
    }

    if (oa_tc6_read_buffer_status(priv->tc6, &txc, &rba)) {
        return;
    }
    if (txc) {
        /* The chip has credits but the last footer said 0 and no IRQ
         * came: re-run a data transfer instead of resetting.
         */
        netdev_warn(priv->netdev, "TX credits %u available but not reported, kicking the data path\n", txc);
        priv->hwstats[LAN865X_SW_KICKS]++;
        priv->stall_since = 0;
        oa_tc6_kick(priv->tc6);
        return;
    }

    priv->hwstats[LAN865X_SW_STALL_DETECTED]++;
    lan865x_trigger_recovery(priv, LAN865X_RCV_STALL);
}

/* One line with everything needed to tell chip from driver (docs/notes/32
 * section 2.5): BUFSTS.TXC = 0 with XCOL > 0 means the chip.
 */
static void lan865x_dump_state(struct lan865x_priv* priv, const char* when) {
    struct oa_tc6_tx_state st;
    u32 st01[2] = { 0 }, bufsts = 0, ncr = 0, plca = 0, cd = 0;

    oa_tc6_get_tx_state(priv->tc6, &st);
    oa_tc6_read_registers(priv->tc6, 0x00000008, st01, 2); /* OA_STATUS0/1 */
    oa_tc6_read_register(priv->tc6, 0x0000000B, &bufsts);  /* OA_BUFSTS */
    oa_tc6_read_register(priv->tc6, LAN865X_MMS1_MAC_NCR, &ncr);
    oa_tc6_read_register(priv->tc6, LAN865X_MMS4_PLCA_STS, &plca);
    oa_tc6_read_register(priv->tc6, LAN865X_MMS4_CDCTL0, &cd);
    netdev_warn(priv->netdev,
                "%s: STATUS0=0x%08x STATUS1=0x%08x BUFSTS=0x%08x (TXC=%u RBA=%u) NCR=0x%08x PLCA_STS=0x%04x "
                "CDCTL0=0x%04x | footer TXC=%u pending=%d | XCOL=%llu TFTX=%llu FTX=%llu CORTX=%llu tx_packets=%lu\n",
                when, st01[0], st01[1], bufsts, (bufsts >> 8) & 0xFF, bufsts & 0xFF, ncr, plca, cd, st.tx_credits,
                st.tx_pending, priv->hwstats[LAN865X_HW_XCOL], priv->hwstats[LAN865X_HW_TFTX],
                priv->hwstats[LAN865X_HW_FTX], priv->hwstats[LAN865X_HW_CORTX], priv->netdev->stats.tx_packets);
}

static bool lan865x_try_txen_toggle(struct lan865x_priv* priv) {
    u32 ncr;
    u8 txc = 0, rba;

    if (oa_tc6_read_register(priv->tc6, LAN865X_MMS1_MAC_NCR, &ncr)) {
        return false;
    }
    if (oa_tc6_write_register(priv->tc6, LAN865X_MMS1_MAC_NCR, ncr & ~LAN865X_MAC_NCR_TXEN) ||
        oa_tc6_write_register(priv->tc6, LAN865X_MMS1_MAC_NCR, ncr | LAN865X_MAC_NCR_TXEN)) {
        return false;
    }
    msleep(10);
    if (oa_tc6_read_buffer_status(priv->tc6, &txc, &rba)) {
        return false;
    }
    netdev_warn(priv->netdev, "after MAC_NCR.TXEN toggle: BUFSTS.TXC=%u\n", txc);
    return txc == LAN865X_TXC_MAX;
}

static void lan865x_rcv_work(struct work_struct* work) {
    struct lan865x_priv* priv = container_of(to_delayed_work(work), struct lan865x_priv, rcv_work);
    struct net_device* netdev = priv->netdev;
    struct phy_device* phydev = netdev->phydev;
    int reason = priv->rcv_reason;
    bool ok = false;
    int ret = 0;

    rtnl_lock();
    mutex_lock(&priv->hw_lock);

    if (test_bit(LAN865X_RCV_STOPPING, &priv->rcv_flags)) {
        goto out;
    }

    lan865x_poll_hw_stats(priv); /* keep the counts the reset would clear */
    lan865x_dump_state(priv, "before recovery");

    if (recovery_mode == 0) {
        netdev_warn(netdev, "recovery_mode=0: not recovering (%s)\n", lan865x_rcv_reason_str[reason]);
        lan865x_rebaseline(priv);
        goto out;
    }

    if (!test_and_set_bit(LAN865X_RCV_PARKED, &priv->rcv_flags)) {
        /* not again when retrying after a failed attempt */
        priv->rcv_phy_started = phydev && phy_is_started(phydev);
        netif_tx_disable(netdev);
        if (priv->rcv_phy_started) {
            phy_stop(phydev);
        }
        oa_tc6_stop_traffic(priv->tc6);
    }

    if (recovery_mode == 1 && reason != LAN865X_RCV_FATAL) {
        ok = lan865x_try_txen_toggle(priv);
        if (ok) {
            priv->hwstats[LAN865X_SW_RCV_LIGHT_OK]++;
        }
    }

    if (!ok) {
        ret = oa_tc6_hw_reinit(priv->tc6);
        if (!ret) {
            ret = lan865x_restore_hw_config(priv);
        }
        if (!ret) {
            priv->hwstats[LAN865X_SW_RCV_RESET_OK]++;
            ok = true;
        }
    }

    if (!ok) {
        /* Leave the data path parked and try again (with back-off). */
        priv->hwstats[LAN865X_SW_RCV_FAILED]++;
        netdev_err(netdev, "recovery failed: %d\n", ret);
        priv->rcv_last = jiffies;
        clear_bit(LAN865X_RCV_PENDING, &priv->rcv_flags);
        mutex_unlock(&priv->hw_lock);
        rtnl_unlock();
        lan865x_trigger_recovery(priv, LAN865X_RCV_FATAL);
        return;
    }

    oa_tc6_start_traffic(priv->tc6);
    clear_bit(LAN865X_RCV_PARKED, &priv->rcv_flags);
    if (priv->rcv_phy_started) {
        phy_start(phydev);
    }
    if (netif_running(netdev)) {
        netif_tx_wake_all_queues(netdev);
    }
    lan865x_rebaseline(priv);
    lan865x_dump_state(priv, "after recovery");
    netdev_warn(netdev, "recovered from %s\n", lan865x_rcv_reason_str[reason]);

out:
    priv->rcv_last = jiffies;
    clear_bit(LAN865X_RCV_PENDING, &priv->rcv_flags);
    mutex_unlock(&priv->hw_lock);
    rtnl_unlock();
}

/* ------------------------------------------------------------------------
 * Collision detection (MMS4 CDCTL0.CDEN) per PLCA state
 *
 * DS60001734F CDCTL0 (11.5): "it is recommended that the user write this bit
 * to a '0' to disable collision detection when PLCA is enabled. When
 * collision detection is disabled, the PLCA reconciliation sublayer will
 * still assert logical collisions to the MAC as part of normal operation."
 * AN1760 Rev F (DS60001760G) and upstream microchip_t1s 78341049fbcd ("net:
 * phy: microchip_t1s: configure collision detection based on PLCA mode")
 * do the same: CDEN = 0 with PLCA, 1 with CSMA/CD.  The SDK always set
 * CDEN = 1 (init_lan865x()), so in the first stall of docs/notes/32 the
 * node's TO overlapping the BEACON was taken as a collision every cycle.
 *
 * But DS60001734F 7.2.4: when the PLCA status (PST) drops "the device will
 * revert to CSMA/CD operation with PLCA deactivated" - and then collisions
 * must be detected again.  Hence:
 *   cden_mode=0  CDEN always 1 (SDK behaviour)
 *   cden_mode=1  CDEN = !PLCA_CTRL0.EN (AN1760 / upstream, static)
 *   cden_mode=2  CDEN = !(PLCA_CTRL0.EN && PLCA_STS.PST) (default): on as
 *                soon as PST is seen 0, off after PST was 1 in two
 *                consecutive polls.  While PLCA is lost and before the
 *                next poll (<= stats_ms) collisions go undetected.
 */
static unsigned int cden_mode = 2;
module_param(cden_mode, uint, 0644);
MODULE_PARM_DESC(cden_mode, "Collision detect: 0 = always on (SDK), 1 = off while PLCA enabled, 2 = off while PLCA enabled and PST=1 (default)");

#define LAN865X_MMS4_PLCA_CTRL0 0x0004CA01 /* CTRL0, CTRL1, STS consecutive */
#define LAN865X_PLCA_CTRL0_EN BIT(15)
#define LAN865X_PLCA_STS_PST BIT(15)
#define LAN865X_CDCTL0_CDEN BIT(15)

static void lan865x_apply_cden(struct lan865x_priv* priv) {
    u32 plca[3]; /* PLCA_CTRL0, PLCA_CTRL1, PLCA_STS */
    bool en, pst;
    int want;
    u32 cd;

    if (oa_tc6_read_registers(priv->tc6, LAN865X_MMS4_PLCA_CTRL0, plca, 3)) {
        priv->hwstats[LAN865X_SW_POLL_ERRORS]++;
        return;
    }
    en = plca[0] & LAN865X_PLCA_CTRL0_EN;
    pst = plca[2] & LAN865X_PLCA_STS_PST;
    priv->hwstats[LAN865X_SW_PLCA_PST] = pst;
    priv->pst_ok_polls = (en && pst) ? priv->pst_ok_polls + 1 : 0;

    switch (cden_mode) {
    case 0:
        want = 1;
        break;
    case 1:
        want = !en;
        break;
    default:
        if (!en || !pst) {
            want = 1;
        } else if (priv->pst_ok_polls >= 2) {
            want = 0;
        } else {
            want = priv->cden_cur < 0 ? 1 : priv->cden_cur;
        }
        break;
    }

    if (want == priv->cden_cur) {
        return;
    }

    if (oa_tc6_read_register(priv->tc6, LAN865X_MMS4_CDCTL0, &cd)) {
        priv->hwstats[LAN865X_SW_POLL_ERRORS]++;
        return;
    }
    /* read-modify-write: reserved fields (CDCTL0 note) */
    cd = want ? (cd | LAN865X_CDCTL0_CDEN) : (cd & ~LAN865X_CDCTL0_CDEN);
    if (oa_tc6_write_register(priv->tc6, LAN865X_MMS4_CDCTL0, cd)) {
        priv->hwstats[LAN865X_SW_POLL_ERRORS]++;
        return;
    }
    if (priv->cden_cur >= 0) {
        priv->hwstats[LAN865X_SW_CDEN_CHANGES]++;
        netdev_info(priv->netdev, "collision detection %s (PLCA %s, PST=%d, cden_mode=%u)\n",
                    want ? "enabled" : "disabled", en ? "enabled" : "disabled", pst, cden_mode);
    }
    priv->cden_cur = want;
    priv->hwstats[LAN865X_SW_CDEN] = want;
}

static void lan865x_mon_work(struct work_struct* work) {
    struct lan865x_priv* priv = container_of(to_delayed_work(work), struct lan865x_priv, mon_work);

    mutex_lock(&priv->hw_lock);
    lan865x_poll_hw_stats(priv);
    lan865x_check_tx(priv);
    if (!test_bit(LAN865X_RCV_PENDING, &priv->rcv_flags)) {
        lan865x_apply_cden(priv);
    }
    mutex_unlock(&priv->hw_lock);

    if (stats_ms) {
        schedule_delayed_work(&priv->mon_work, msecs_to_jiffies(stats_ms));
    }
}

int lan865x_health_init(struct lan865x_priv* priv) {
    mutex_init(&priv->hw_lock);
    spin_lock_init(&priv->stats_lock);
    INIT_DELAYED_WORK(&priv->mon_work, lan865x_mon_work);
    INIT_DELAYED_WORK(&priv->rcv_work, lan865x_rcv_work);
    /* before register_netdev(): dev_activate() picks it up on open */
    priv->netdev->watchdog_timeo = msecs_to_jiffies(tx_stall_ms ? tx_stall_ms : 2000);
    return 0;
}

void lan865x_health_start(struct lan865x_priv* priv) {
    lan865x_rebaseline(priv);
    oa_tc6_set_fatal_cb(priv->tc6, lan865x_fatal_cb, priv);
    if (stats_ms) {
        schedule_delayed_work(&priv->mon_work, msecs_to_jiffies(stats_ms));
    }
}

void lan865x_health_stop(struct lan865x_priv* priv) {
    set_bit(LAN865X_RCV_STOPPING, &priv->rcv_flags);
    if (priv->tc6) {
        oa_tc6_set_fatal_cb(priv->tc6, NULL, NULL);
    }
    cancel_delayed_work_sync(&priv->mon_work);
    cancel_delayed_work_sync(&priv->rcv_work);
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
