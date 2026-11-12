// SPDX-License-Identifier: GPL-2.0+
/*
 * TSN Lab T1S MAC-PHY: the device specific part of the LAN865x driver.
 * See t1s_oaspi.h.  Register map: t1s repo mac/rtl/t1s_mac_regs.v (MMS 1).
 *
 * LAN865x MMS1 access            -> this device
 *   NCR 0x00 .3 TXEN .2 RXEN     -> MAC_CTRL 0x01 .0 tx_en .1 rx_en (RMW)
 *   NCFGR 0x01 .4 CAF            -> MAC_CTRL 0x01 .4 promisc
 *   NCFGR 0x01 .6 MTIHEN .7 UNIHEN -> none (group hash always on, no
 *                                    unicast hash); hash all 0 = no groups
 *   HRB/HRT 0x20/0x21 (6-bit XOR fold index)
 *                                -> MCAST_HASH_L/H 0x05/0x06, index =
 *                                   ether_crc_le(6, da) & 0x3f
 *   SAB1/SAT1 0x22/0x23, SAB2/SAT2 0x24/0x25 (first octet in bits 7:0)
 *                                -> MAC_ADDR_L/H 0x02/0x03 (first octet in
 *                                   ADDR_H bits 15:8)
 *   SAB1 back-off seed (lab-fixes backoff_seed) -> none, skipped
 *   TI/TISUBN/TSH/TSL/TN/TA 0x77/0x6F/0x70/0x74/0x75/0x76 (PTP, TS build)
 *                                -> PTP_* 0x20..0x2A (command model), own
 *                                   PHC in t1s_ptp.c (TIMESTAMP=0 build)
 *   STATS0..12 0x208..0x214, BMGR_CTL 0x200 -> statistics 0x40..0x5A
 *                                   (t1s_oaspi_poll_stats; 802.3 30.3.1.1
 *                                   names, 32 bit; address >= 0x100 not used)
 * LAN865x MMS4 vendor writes (AN1760 trims 0x0040..0x00F9, CDCTL0 0x0087,
 * indirect 0x00D8..0x00DA, STS1 0x0018) are not sent: this PHY keeps its
 * collision detect control in 31.D106 (reset value = product default).
 */
#include "t1s_oaspi.h"

#include <linux/crc32.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/skbuff.h>
#include <linux/spi/spi.h>
#include <linux/sysfs.h>

#include "../../../include/linux/oa_tc6.h"
#include "lan865x_arch.h"
#include "lan865x_health.h"
#include "lan865x_variant.h"

/* TC6 MMS0 (OPEN Alliance TC6 v1.1 9.2) */
#define T1S_TC6_STDCAP 0x00000002
#define T1S_TC6_STDCAP_TXFCSVC BIT(10) /* transmit FCS validation capability (Table 10) */
#define T1S_TC6_CONFIG0 0x00000004
#define T1S_TC6_CONFIG0_TXFCSVE BIT(14) /* transmit FCS validation enable */
#define T1S_TC6_CONFIG0_ZARFE BIT(12)
#define T1S_TC6_STATUS0 0x00000008
#define T1S_TC6_IMASK0 0x0000000C
#define T1S_TC6_IMASK0_TXFCSEM BIT(11) /* STATUS0.TXFCSE mask */

/* standard PLCA (OA PLCA management registers, MMS4 = MMD 31) */
#define T1S_MMS4_PLCA_CTRL0 0x0004CA01
#define T1S_PLCA_CTRL0_EN BIT(15)
#define T1S_MMS4_PLCA_CTRL1 0x0004CA02
#define T1S_PLCA_NODE_COUNT 8 /* PLCA_CTRL1[15:8], as lan865x_set_nodeid() */

static unsigned int t1s_spi_hz;
module_param(t1s_spi_hz, uint, 0444);
MODULE_PARM_DESC(t1s_spi_hz, "t1s-oaspi: SPI clock in Hz (0 = DT spi-max-frequency, else 12.5 MHz)");

/* Transmit FCS validation (TC6 v1.1 CONFIG0.TXFCSVE): the host pads every
 * frame to 60 bytes and appends the FCS, the MAC-PHY checks it and reports a
 * bad one in STATUS0.TXFCSE (ethtool -S oa_tx_fcs_errors, kernel log).  Only
 * when the MAC-PHY has STDCAP.TXFCSVC = 1, and only in the TIMESTAMP=0 build
 * (t1s_oaspi_xmit() is the transmit path that appends the FCS).
 */
static unsigned int t1s_txfcs;
module_param(t1s_txfcs, uint, 0444);
MODULE_PARM_DESC(t1s_txfcs, "t1s-oaspi: 1 = host appends the TX FCS and the MAC-PHY checks it (CONFIG0.TXFCSVE, needs STDCAP.TXFCSVC); "
                            "also ethtool --set-priv-flags <if> tx-fcs-check on|off while the interface is down. Default 0");

#ifndef FRAME_TIMESTAMP_ENABLE
#define T1S_TXFCS_BUILD true
#else
#define T1S_TXFCS_BUILD false
#endif

static int t1s_rxfcs_setup(struct oa_tc6* tc6, struct lan865x_priv* priv) {
    bool on;
    u32 v;
    int ret;

    ret = oa_tc6_write_register(tc6, T1S_MMS1_MAC_RXCFG, T1S_MAC_RXCFG_RXFCSP);
    if (ret) {
        return ret;
    }
    ret = oa_tc6_read_register(tc6, T1S_MMS1_MAC_RXCFG, &v);
    if (ret) {
        return ret;
    }
    on = !!(v & T1S_MAC_RXCFG_RXFCSP);
#ifndef FRAME_TIMESTAMP_ENABLE
    oa_tc6_set_rx_fcs(tc6, on ? ETH_FCS_LEN : 0);
    if (on) {
        struct net_device* nd = oa_tc6_get_netdev(tc6);

        nd->hw_features |= NETIF_F_RXFCS; /* ethtool -K rx-fcs on keeps it */
    }
#else
    /* TIMESTAMP=1 build: oa_tc6_prcs_rx_frame_end() always cuts the last 4 bytes
     * of a received frame, so NETIF_F_RXFCS is not offered (ethtool -K rx-fcs on
     * would change nothing), and that cut is right only while the MAC passes the
     * FCS. A MAC that reads RXFCSP back as 0 already strips it: say so.
     */
    if (!on) {
        dev_warn(&priv->spi->dev,
                 "t1s-oaspi: MAC_RXCFG.RXFCSP reads 0 (0x%08x) but the TIMESTAMP=1 build always cuts 4 bytes: received frames lose their last 4 bytes\n",
                 v);
    }
#endif
    if (on != priv->t1s_rxfcs) {
        dev_info(&priv->spi->dev, "t1s-oaspi: received FCS %s\n",
                 on ? "passed by the MAC, stripped by the driver" : "stripped by the MAC (no MAC_RXCFG)");
    }
    priv->t1s_rxfcs = on;
    return 0;
}

/* CONFIG0.TXFCSVE and the STATUS0.TXFCSE interrupt mask for priv->t1s_txfcs_req.
 * *config0 is the CONFIG0 value to write (TXFCSVE set or cleared here).
 */
static int t1s_txfcs_setup(struct oa_tc6* tc6, struct lan865x_priv* priv, u32* config0) {
    u32 cap, imask;
    bool on;
    int ret;

    ret = oa_tc6_read_register(tc6, T1S_TC6_STDCAP, &cap);
    if (ret) {
        return ret;
    }
    on = priv->t1s_txfcs_req && T1S_TXFCS_BUILD && (cap & T1S_TC6_STDCAP_TXFCSVC);
    if (priv->t1s_txfcs_req && !on) {
        dev_warn(&priv->spi->dev, "t1s-oaspi: TX FCS check not available (STDCAP 0x%08x, TXFCSVC %u%s), host FCS off\n", cap,
                 !!(cap & T1S_TC6_STDCAP_TXFCSVC), T1S_TXFCS_BUILD ? "" : ", TIMESTAMP=1 build");
    }

    ret = oa_tc6_read_register(tc6, T1S_TC6_IMASK0, &imask);
    if (ret) {
        return ret;
    }
    imask = on ? (imask & ~T1S_TC6_IMASK0_TXFCSEM) : (imask | T1S_TC6_IMASK0_TXFCSEM);
    ret = oa_tc6_write_register(tc6, T1S_TC6_IMASK0, imask);
    if (ret) {
        return ret;
    }

    *config0 = on ? (*config0 | T1S_TC6_CONFIG0_TXFCSVE) : (*config0 & ~T1S_TC6_CONFIG0_TXFCSVE);
    if (on != READ_ONCE(priv->t1s_txfcs)) {
        dev_info(&priv->spi->dev, "t1s-oaspi: TX FCS check %s (CONFIG0.TXFCSVE)\n", on ? "on, host appends the FCS" : "off");
    }
    WRITE_ONCE(priv->t1s_txfcs, on);
    return 0;
}

int t1s_oaspi_probe_cfg(struct lan865x_priv* priv, struct spi_device* spi) {
    u32 v = T1S_DEF_NODE_ID;
    u32 hz;

    of_property_read_u32(spi->dev.of_node, "tsnlab,plca-node-id", &v);
    priv->node_id = v & 0xFF;
    priv->t1s_txfcs_req = !!t1s_txfcs;

    /* TC6 v1.1 7.1: SPI mode 0 (CPOL 0, CPHA 0) */
    hz = t1s_spi_hz ? t1s_spi_hz : (spi->max_speed_hz ? spi->max_speed_hz : T1S_DEF_SPI_HZ);
    spi->mode = SPI_MODE_0;
    spi->bits_per_word = 8;
    spi->max_speed_hz = hz;

    dev_info(&spi->dev, "t1s-oaspi: SPI mode 0, %u Hz, PLCA node id %u (DT)\n", hz, priv->node_id);
    return 0;
}

static int t1s_write_macaddr(struct oa_tc6* tc6, const u8* mac) {
    u32 lo = (mac[2] << 24) | (mac[3] << 16) | (mac[4] << 8) | mac[5];
    u32 hi = (mac[0] << 8) | mac[1];
    int ret;

    ret = oa_tc6_write_register(tc6, T1S_MMS1_MAC_ADDR_L, lo);
    if (ret) {
        return ret;
    }
    return oa_tc6_write_register(tc6, T1S_MMS1_MAC_ADDR_H, hi);
}

/* Replaces init_lan865x(): TC6 standard steps + this MAC only.
 * TC6 v1.1 7.6: the MAC-PHY is configured before CONFIG0.SYNC is set (by
 * oa_tc6_enable_data_transfer() after this hook): PLCA node id before PLCA
 * enable, MAC address, zero-aligned receive frames.  Also the recovery path
 * (oa_tc6_hw_reinit()), so the node id and MAC address are never at their
 * reset values while the MAC-PHY is synchronized.
 */
int t1s_oaspi_chip_init(struct oa_tc6* tc6) {
    struct net_device* nd = oa_tc6_get_netdev(tc6);
    struct lan865x_priv* priv = netdev_priv(nd);
    u32 v;
    int ret;

    /* RESETC (and any other latched STATUS0 bit) W1C */
    ret = oa_tc6_read_register(tc6, T1S_TC6_STATUS0, &v);
    if (ret) {
        return ret;
    }
    ret = oa_tc6_write_register(tc6, T1S_TC6_STATUS0, v);
    if (ret) {
        return ret;
    }

    /* PLCA node id (DT) first, then PLCA enabled */
    ret = oa_tc6_write_register(tc6, T1S_MMS4_PLCA_CTRL1, (T1S_PLCA_NODE_COUNT << 8) | priv->node_id);
    if (ret) {
        return ret;
    }
    ret = oa_tc6_write_register(tc6, T1S_MMS4_PLCA_CTRL0, T1S_PLCA_CTRL0_EN);
    if (ret) {
        return ret;
    }

    /* MAC address (netdev: set by the probe, or changed since, before a recovery) */
    if (is_valid_ether_addr(nd->dev_addr)) {
        ret = t1s_write_macaddr(tc6, nd->dev_addr);
        if (ret) {
            return ret;
        }
    }

    /* The MAC drops longer received frames: its reset value 1518 (DA..FCS)
     * loses 802.1Q tagged frames of full size that Linux sends (1522).
     * Set again by every recovery (oa_tc6_hw_reinit()).
     */
    ret = oa_tc6_write_register(tc6, T1S_MMS1_MAX_FRAME, T1S_MAX_FRAME);
    if (ret) {
        return ret;
    }

    /* Received FCS: MAC_RXCFG.RXFCSP = 1 (also its reset value) makes the
     * MAC pass every frame as received, pad and FCS included, like the
     * LAN865x; oa_tc6 strips the FCS unless NETIF_F_RXFCS (ethtool -K rx-fcs
     * on).  A MAC without the register (older bitstreams, TEMAC board
     * builds) reads 0 there and strips the FCS itself: nothing to strip.
     */
    ret = t1s_rxfcs_setup(tc6, priv);
    if (ret) {
        return ret;
    }

    /* same as LAN865x NCR = TXEN | RXEN, NCFGR = no promiscuous */
    ret = oa_tc6_read_register(tc6, T1S_MMS1_MAC_CTRL, &v);
    if (ret) {
        return ret;
    }
    v = (v | T1S_MAC_CTRL_TXEN | T1S_MAC_CTRL_RXEN) & ~T1S_MAC_CTRL_PROMISC;
    ret = oa_tc6_write_register(tc6, T1S_MMS1_MAC_CTRL, v);
    if (ret) {
        return ret;
    }

    /* MAC timestamps (TS_CFG, latencies, clock rate) and the receive
     * layout before SYNC: no frame crosses the change (t1s_ptp.c)
     */
    ret = t1s_ptp_chip_init(tc6, priv);
    if (ret) {
        return ret;
    }

    /* CONFIG0.ZARFE; SYNC is set by oa_tc6 after this hook.  CONFIG0.FTSE/FTSS
     * stay 0: the receive timestamp is configured in the MAC (TS_CFG; with
     * STDCAP.FTSC = 1 CONFIG0.FTSE / FTSS are an alias of it), see t1s_ptp.c
     */
    ret = oa_tc6_read_register(tc6, T1S_TC6_CONFIG0, &v);
    if (ret) {
        return ret;
    }
    /* CONFIG0.TXFCSVE before SYNC too (TC6 v1.1 7.6), and IMASK0.TXFCSEM */
    ret = t1s_txfcs_setup(tc6, priv, &v);
    if (ret) {
        return ret;
    }
    ret = oa_tc6_write_register(tc6, T1S_TC6_CONFIG0, v | T1S_TC6_CONFIG0_ZARFE);
    if (ret) {
        return ret;
    }

    ret = oa_tc6_read_register(tc6, T1S_TC6_STATUS0, &v);
    if (ret) {
        return ret;
    }
    return oa_tc6_write_register(tc6, T1S_TC6_STATUS0, v);
}

int t1s_oaspi_set_macaddr(struct lan865x_priv* priv, const u8* mac) {
    return t1s_write_macaddr(priv->tc6, mac);
}

int t1s_oaspi_mac_enable(struct lan865x_priv* priv, bool en) {
    u32 v;

    if (oa_tc6_read_register(priv->tc6, T1S_MMS1_MAC_CTRL, &v)) {
        return -ENODEV;
    }
    if (en) {
        v |= T1S_MAC_CTRL_TXEN | T1S_MAC_CTRL_RXEN;
    } else {
        v &= ~(T1S_MAC_CTRL_TXEN | T1S_MAC_CTRL_RXEN);
    }
    if (oa_tc6_write_register(priv->tc6, T1S_MMS1_MAC_CTRL, v)) {
        return -ENODEV;
    }
    return 0;
}

/* t1s_mac_filter.v: broadcast always, own address, group addresses whose
 * hash bit is set, everything when promisc.
 */
void t1s_oaspi_set_rx_mode(struct lan865x_priv* priv) {
    struct net_device* nd = priv->netdev;
    struct netdev_hw_addr* ha;
    u32 hash[2] = {0, 0};
    u32 ctrl;
    bool promisc = nd->flags & IFF_PROMISC;

    if (nd->flags & IFF_ALLMULTI) {
        hash[0] = 0xFFFFFFFF;
        hash[1] = 0xFFFFFFFF;
    } else {
        netdev_for_each_mc_addr(ha, nd) {
            u32 idx = ether_crc_le(ETH_ALEN, ha->addr) & 0x3F;

            hash[idx >> 5] |= BIT(idx & 31);
        }
    }

    if (oa_tc6_write_register(priv->tc6, T1S_MMS1_MCAST_HASH_L, hash[0]) ||
        oa_tc6_write_register(priv->tc6, T1S_MMS1_MCAST_HASH_H, hash[1]) ||
        oa_tc6_read_register(priv->tc6, T1S_MMS1_MAC_CTRL, &ctrl)) {
        netdev_err(nd, "t1s-oaspi: rx mode update failed\n");
        return;
    }
    ctrl = promisc ? (ctrl | T1S_MAC_CTRL_PROMISC) : (ctrl & ~T1S_MAC_CTRL_PROMISC);
    if (oa_tc6_write_register(priv->tc6, T1S_MMS1_MAC_CTRL, ctrl)) {
        netdev_err(nd, "t1s-oaspi: MAC_CTRL write failed\n");
    }
}

static const struct {
    u32 addr;
    const char* name;
} t1s_dump_regs[] = {
    {0x00000000, "OA_ID"},     {0x00000001, "OA_PHYID"},  {0x00000002, "OA_STDCAP"},  {0x00000004, "OA_CONFIG0"},
    {0x00000008, "OA_STATUS0"}, {0x00000009, "OA_STATUS1"}, {0x0000000B, "OA_BUFSTS"}, {0x0000000C, "OA_IMASK0"},
    {0x0000000D, "OA_IMASK1"}, {0x00010000, "MAC_ID"},    {0x00010001, "MAC_CTRL"},   {0x00010002, "MAC_ADDR_L"},
    {0x00010003, "MAC_ADDR_H"}, {0x00010005, "MC_HASH_L"}, {0x00010006, "MC_HASH_H"}, {0x00010007, "MAC_CAP"}, {0x0001000A, "MAC_RXCFG"},
    {0x00010008, "MAC_IRQ_STS"}, {0x00010016, "TS_CFG"},   {0x00010017, "TS_STS"},     {0x00010018, "TS_IMSK"},
    {0x00010019, "TS_TX_LAT"},  {0x0001001A, "TS_RX_LAT"}, {0x00010020, "PTP_CMD"},    {0x00010024, "PTP_INC"},
    {0x00010010, "TTSCAH"},     {0x00010011, "TTSCAL"},    {0x00010012, "TTSCBH"},     {0x00010013, "TTSCBL"},
    {0x00010014, "TTSCCH"},     {0x00010015, "TTSCCL"},    {0x00010027, "PPS_SSEC"},   {0x00010028, "PPS_SNS"},
    {0x00010029, "PPS_PERIOD"}, {0x0001002A, "PPS_WIDTH"}, {0x0004CA00, "MIDVER"},     {0x0004CA01, "PLCA_CTRL0"},
    {0x0004CA02, "PLCA_CTRL1"}, {0x0004CA03, "PLCA_STS"},  {0x0004CA04, "PLCA_TOTMR"}, {0x0004CA05, "PLCA_BURST"},
};

ssize_t t1s_oaspi_regdump(struct lan865x_priv* priv, char* buf) {
    ssize_t n = 0;
    u32 v;
    int ret;

    for (int i = 0; i < ARRAY_SIZE(t1s_dump_regs); i++) {
        u32 a = t1s_dump_regs[i].addr;

        ret = oa_tc6_read_register(priv->tc6, a, &v);
        if (ret) {
            n += sysfs_emit_at(buf, n, "%02u:%04x %-12s ERR %d\n", a >> 16, a & 0xFFFF, t1s_dump_regs[i].name, ret);
        } else {
            n += sysfs_emit_at(buf, n, "%02u:%04x %-12s 0x%08x\n", a >> 16, a & 0xFFFF, t1s_dump_regs[i].name, v);
        }
    }
    return t1s_ptp_stats(priv, buf, n);
}

/* ------------------------------------------------------------- statistics */

/* ethtool -S names of MMS1 0x40..0x5A, in address order (802.3 30.3.1.1
 * attribute in brackets where there is one)
 */
const char t1s_oaspi_stat_names[LAN865X_T1S_NSTATS][32] = {
    "mac_tx_frames_ok",               /* 0x40 aFramesTransmittedOK */
    "mac_tx_single_collision_frames", /* 0x41 aSingleCollisionFrames */
    "mac_tx_multi_collision_frames",  /* 0x42 aMultipleCollisionFrames */
    "mac_tx_deferred_frames",         /* 0x43 aFramesWithDeferredXmissions */
    "mac_tx_late_collisions",         /* 0x44 aLateCollisions */
    "mac_tx_excessive_collisions",    /* 0x45 aFramesAbortedDueToXSColls */
    "mac_tx_internal_mac_errors",     /* 0x46 aFramesLostDueToIntMACXmitError */
    "mac_tx_excessive_deferral",      /* 0x47 aFramesWithExcessiveDeferral */
    "mac_tx_collisions",              /* 0x48 collisions seen while sending */
    "mac_rx_frames_ok",               /* 0x49 aFramesReceivedOK */
    "mac_rx_octets_ok",               /* 0x4A aOctetsReceivedOK */
    "mac_rx_fcs_errors",              /* 0x4B aFrameCheckSequenceErrors */
    "mac_rx_alignment_errors",        /* 0x4C aAlignmentErrors */
    "mac_rx_frame_too_long",          /* 0x4D aFrameTooLongErrors */
    "mac_rx_multicast_frames",        /* 0x4E aMulticastFramesReceivedOK */
    "mac_rx_broadcast_frames",        /* 0x4F aBroadcastFramesReceivedOK */
    "mac_rx_internal_mac_errors",     /* 0x50 aFramesLostDueToIntMACRcvError */
    "mac_rx_runt_frames",             /* 0x51 */
    "mac_rx_filtered_frames",         /* 0x52 */
    "mac_tx_octets_ok",               /* 0x53 aOctetsTransmittedOK */
    "mac_tx_multicast_frames",        /* 0x54 aMulticastFramesXmittedOK */
    "mac_tx_broadcast_frames",        /* 0x55 aBroadcastFramesXmittedOK */
    "mac_carrier_sense_errors",       /* 0x56 aCarrierSenseErrors */
    "mac_rx_in_range_length_errors",  /* 0x57 aInRangeLengthErrors */
    "mac_rx_out_of_range_length",     /* 0x58 aOutOfRangeLengthField */
    "phy_collisions",                 /* 0x59 physical collisions (PHY COL while sending) */
    "mac_tx_frame_too_long",          /* 0x5A host TX frames dropped for length */
};

/* Read and accumulate the clear-on-read statistics.  Called with hw_lock. */
void t1s_oaspi_poll_stats(struct lan865x_priv* priv) {
    u32 st[LAN865X_T1S_NSTATS];
    int ret;

    ret = oa_tc6_read_registers(priv->tc6, T1S_MMS1_STAT0, st, LAN865X_T1S_NSTATS);
    spin_lock_bh(&priv->stats_lock);
    if (ret) {
        priv->hwstats[LAN865X_SW_POLL_ERRORS]++;
    } else {
        for (int i = 0; i < LAN865X_T1S_NSTATS; i++) {
            priv->t1s_stats[i] += st[i];
            if (st[i] == 0xFFFFFFFFu) {
                priv->hwstats[LAN865X_SW_SATURATED]++;
            }
        }
    }
    spin_unlock_bh(&priv->stats_lock);
}

/* ---------------------------------------------------------------- variant */

/* oa_tc6 part: the init step only (no vendor STATUS1 / IMASK1 bits) */
/* LOFE unmasked by default: the t1s-oaspi LOFE means a short chunk / CSn glitch
 * only (TC6 v1.1 7.5.2), so the frame it dropped is sent again.
 */
static const struct oa_tc6_chip_ops t1s_oaspi_tc6_ops = {
    .chip_init = t1s_oaspi_chip_init,
    .lofe_unmask = true,
};

#ifndef FRAME_TIMESTAMP_ENABLE
/* CONFIG0.TXFCSVE = 1: pad to the minimum frame (60 bytes without FCS, as the
 * MAC does when it adds the FCS itself) and append the FCS (802.3 3.2.9:
 * CRC-32, complemented, least significant byte first).  Returns 0, or -ENOMEM
 * with the skb untouched.
 */
static int t1s_txfcs_append(struct sk_buff* skb) {
    unsigned int pad = skb->len < ETH_ZLEN ? ETH_ZLEN - skb->len : 0;
    __le32 fcs;

    if (skb_linearize(skb)) {
        return -ENOMEM;
    }
    if (skb_cloned(skb) || skb_tailroom(skb) < pad + ETH_FCS_LEN) {
        if (pskb_expand_head(skb, 0, pad + ETH_FCS_LEN, GFP_ATOMIC)) {
            return -ENOMEM;
        }
    }
    if (pad) {
        skb_put_zero(skb, pad);
    }
    fcs = cpu_to_le32(~crc32_le(~0, skb->data, skb->len));
    skb_put_data(skb, &fcs, ETH_FCS_LEN);
    return 0;
}

static netdev_tx_t t1s_oaspi_xmit(struct lan865x_priv* priv, struct sk_buff* skb) {
    unsigned int len0 = skb->len;
    bool fcs = READ_ONCE(priv->t1s_txfcs);
    netdev_tx_t rc;
    u8 tsc;

    if (fcs && t1s_txfcs_append(skb)) {
        priv->netdev->stats.tx_dropped++;
        dev_kfree_skb_any(skb);
        return NETDEV_TX_OK;
    }
    tsc = t1s_ptp_tx_request(priv, skb);

    /* Software TX timestamp (SOF_TIMESTAMPING_TX_SOFTWARE) only for a
     * frame that was queued: on NETDEV_TX_BUSY the stack sends the same
     * skb again, and a second timestamp would sit in the socket error
     * queue (ptp4l: "error on fda[0]", port FAULTY).  The extra reference
     * keeps the skb while the SPI thread may already be done with it.
     */
    skb_get(skb);
    rc = oa_tc6_start_xmit_tsc(priv->tc6, skb, tsc);
    if (rc == NETDEV_TX_BUSY) {
        t1s_ptp_tx_cancel(priv, tsc);
        /* the stack sends the same skb again: no second pad / FCS */
        if (fcs) {
            skb_trim(skb, len0);
        }
    } else {
        skb_tx_timestamp(skb);
    }
    consume_skb(skb);
    return rc;
}

/* own PHC and frame timestamps (t1s_ptp.c); the LAN865x path of a
 * TIMESTAMP=0 build keeps lan865x_ethtool_ops (no get_ts_info)
 */
static int t1s_ethtool_get_ts_info(struct net_device* netdev, LAN865X_TS_INFO* ts_info) {
    return t1s_ptp_get_ts_info(netdev_priv(netdev), ts_info);
}

/* ethtool private flag "tx-fcs-check" = CONFIG0.TXFCSVE (host appends the
 * FCS).  Changed only while the interface is down: no frame is in flight
 * when CONFIG0 changes (SYNC stays set, it is set-only).  A recovery reset
 * applies the last setting again (t1s_oaspi_chip_init()).
 */
static const char t1s_priv_flag_names[][ETH_GSTRING_LEN] = {"tx-fcs-check"};
#define T1S_PRIV_FLAG_TXFCS BIT(0)

static int t1s_ethtool_get_sset_count(struct net_device* netdev, int sset) {
    if (sset == ETH_SS_PRIV_FLAGS) {
        return ARRAY_SIZE(t1s_priv_flag_names);
    }
    return lan865x_get_sset_count(netdev, sset);
}

static void t1s_ethtool_get_strings(struct net_device* netdev, u32 sset, u8* data) {
    if (sset == ETH_SS_PRIV_FLAGS) {
        memcpy(data, t1s_priv_flag_names, sizeof(t1s_priv_flag_names));
        return;
    }
    lan865x_get_strings(netdev, sset, data);
}

static u32 t1s_ethtool_get_priv_flags(struct net_device* netdev) {
    struct lan865x_priv* priv = netdev_priv(netdev);

    return READ_ONCE(priv->t1s_txfcs) ? T1S_PRIV_FLAG_TXFCS : 0;
}

static int t1s_ethtool_set_priv_flags(struct net_device* netdev, u32 flags) {
    struct lan865x_priv* priv = netdev_priv(netdev);
    bool want = flags & T1S_PRIV_FLAG_TXFCS;
    bool old_req = priv->t1s_txfcs_req;
    u32 v;
    int ret;

    if (want == READ_ONCE(priv->t1s_txfcs) && want == old_req) {
        return 0;
    }
    if (netif_running(netdev)) {
        netdev_err(netdev, "t1s-oaspi: tx-fcs-check can only be changed while the interface is down\n");
        return -EBUSY;
    }

    mutex_lock(&priv->hw_lock);
    priv->t1s_txfcs_req = want;
    ret = oa_tc6_read_register(priv->tc6, T1S_TC6_CONFIG0, &v);
    if (!ret) {
        ret = t1s_txfcs_setup(priv->tc6, priv, &v);
    }
    if (!ret) {
        ret = oa_tc6_write_register(priv->tc6, T1S_TC6_CONFIG0, v);
    }
    if (!ret && want && !READ_ONCE(priv->t1s_txfcs)) {
        ret = -EOPNOTSUPP; /* no STDCAP.TXFCSVC (logged by t1s_txfcs_setup()) */
    }
    if (ret) {
        priv->t1s_txfcs_req = old_req && READ_ONCE(priv->t1s_txfcs);
    }
    mutex_unlock(&priv->hw_lock);
    return ret;
}

static const struct ethtool_ops t1s_ethtool_ops = {
    .get_link_ksettings = phy_ethtool_get_link_ksettings,
    .set_link_ksettings = phy_ethtool_set_link_ksettings,
    .get_ts_info = t1s_ethtool_get_ts_info,
    .get_sset_count = t1s_ethtool_get_sset_count,
    .get_strings = t1s_ethtool_get_strings,
    .get_ethtool_stats = lan865x_get_ethtool_stats,
    .get_priv_flags = t1s_ethtool_get_priv_flags,
    .set_priv_flags = t1s_ethtool_set_priv_flags,
};
#endif /* FRAME_TIMESTAMP_ENABLE */

const struct lan865x_variant t1s_oaspi_variant = {
    .chip = LAN865X_CHIP_T1S,
    .irq_gpio = "irq", /* TC6 IRQn, active low */
    .tc6_ops = &t1s_oaspi_tc6_ops,
    .mac_in_chip_init = true,
    .runtime_frame_ts = true,
    .probe_cfg = t1s_oaspi_probe_cfg,
    .set_macaddr = t1s_oaspi_set_macaddr,
    .set_rx_mode = t1s_oaspi_set_rx_mode,
    .mac_enable = t1s_oaspi_mac_enable,
#ifndef FRAME_TIMESTAMP_ENABLE
    .xmit = t1s_oaspi_xmit,
    .hwtstamp_get = t1s_ptp_hwtstamp_get,
    .hwtstamp_set = t1s_ptp_hwtstamp_set,
    .ethtool_ops = &t1s_ethtool_ops,
    .ptp_init = t1s_ptp_init,
    .ptp_exit = t1s_ptp_exit,
#endif /* FRAME_TIMESTAMP_ENABLE */
};
