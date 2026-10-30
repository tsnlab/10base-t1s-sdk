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
 *                                -> PTP_* 0x20..0x2A (command model); not
 *                                   supported: the t1s path needs TIMESTAMP=0
 *   STATS0..12 0x208..0x214, BMGR_CTL 0x200 -> none (statistics 0x40..0x59,
 *                                   not polled; address >= 0x100 not used)
 * LAN865x MMS4 vendor writes (AN1760 trims 0x0040..0x00F9, CDCTL0 0x0087,
 * indirect 0x00D8..0x00DA, STS1 0x0018) are not sent: this PHY keeps its
 * collision detect control in 31.D106 (reset value = product default).
 */
#include "t1s_oaspi.h"

#include <linux/crc32.h>
#include <linux/etherdevice.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/spi/spi.h>
#include <linux/sysfs.h>

#include "lan865x_arch.h"

/* TC6 MMS0 (OPEN Alliance TC6 v1.1 9.2) */
#define T1S_TC6_CONFIG0 0x00000004
#define T1S_TC6_CONFIG0_SYNC BIT(15)
#define T1S_TC6_STATUS0 0x00000008

/* standard PLCA (OA PLCA management registers, MMS4 = MMD 31) */
#define T1S_MMS4_PLCA_CTRL0 0x0004CA01
#define T1S_PLCA_CTRL0_EN BIT(15)

static unsigned int t1s_spi_hz;
module_param(t1s_spi_hz, uint, 0444);
MODULE_PARM_DESC(t1s_spi_hz, "t1s-oaspi: SPI clock in Hz (0 = DT spi-max-frequency, else 12.5 MHz)");

int t1s_oaspi_probe_cfg(struct lan865x_priv* priv, struct spi_device* spi) {
    u32 v = T1S_DEF_NODE_ID;
    u32 hz;

    of_property_read_u32(spi->dev.of_node, "tsnlab,plca-node-id", &v);
    priv->node_id = v & 0xFF;

    /* TC6 v1.1 7.1: SPI mode 0 (CPOL 0, CPHA 0) */
    hz = t1s_spi_hz ? t1s_spi_hz : (spi->max_speed_hz ? spi->max_speed_hz : T1S_DEF_SPI_HZ);
    spi->mode = SPI_MODE_0;
    spi->bits_per_word = 8;
    spi->max_speed_hz = hz;

    dev_info(&spi->dev, "t1s-oaspi: SPI mode 0, %u Hz, PLCA node id %u (DT)\n", hz, priv->node_id);
    return 0;
}

/* Replaces init_lan865x(): TC6 standard steps + this MAC only. */
int t1s_oaspi_chip_init(struct oa_tc6* tc6) {
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

    /* same as the LAN865x path: PLCA enabled, node id set by the probe */
    ret = oa_tc6_write_register(tc6, T1S_MMS4_PLCA_CTRL0, T1S_PLCA_CTRL0_EN);
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

    /* CONFIG0.SYNC; FTSE/FTSS stay 0 (TIMESTAMP=0 build) */
    ret = oa_tc6_read_register(tc6, T1S_TC6_CONFIG0, &v);
    if (ret) {
        return ret;
    }
    ret = oa_tc6_write_register(tc6, T1S_TC6_CONFIG0, v | T1S_TC6_CONFIG0_SYNC);
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
    u32 lo = (mac[2] << 24) | (mac[3] << 16) | (mac[4] << 8) | mac[5];
    u32 hi = (mac[0] << 8) | mac[1];
    int ret;

    ret = oa_tc6_write_register(priv->tc6, T1S_MMS1_MAC_ADDR_L, lo);
    if (ret) {
        return ret;
    }
    return oa_tc6_write_register(priv->tc6, T1S_MMS1_MAC_ADDR_H, hi);
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
    {0x00010003, "MAC_ADDR_H"}, {0x00010005, "MC_HASH_L"}, {0x00010006, "MC_HASH_H"}, {0x00010007, "MAC_CAP"},
    {0x00010008, "MAC_IRQ_STS"}, {0x00010016, "TS_CFG"},   {0x0004CA00, "MIDVER"},     {0x0004CA01, "PLCA_CTRL0"},
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
    return n;
}
