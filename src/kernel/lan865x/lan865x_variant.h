/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * What differs between the MAC-PHYs this driver serves.  lan865x.c is the
 * LAN8650/1 driver; a member left NULL / false means "the LAN8650/1 way in
 * lan865x.c".  Another MAC-PHY (t1s-oaspi: t1s_oaspi.c, t1s_ptp.c) fills in
 * its own.  Selected by the DT / SPI id match data (LAN865X_CHIP_*).
 */
#ifndef LAN865X_VARIANT_H
#define LAN865X_VARIANT_H

#include <linux/netdevice.h>
#include <linux/types.h>
#include <linux/version.h>

/* Linux 6.11 renamed the ethtool get_ts_info argument type */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
#define LAN865X_TS_INFO struct kernel_ethtool_ts_info
#else
#define LAN865X_TS_INFO struct ethtool_ts_info
#endif

struct ethtool_ops;
struct ifreq;
struct lan865x_priv;
struct oa_tc6_chip_ops;
struct sk_buff;
struct spi_device;

struct lan865x_variant {
    u8 chip;                               /* LAN865X_CHIP_* (priv->chip) */
    const char* irq_gpio;                  /* RPi5: DT "<irq_gpio>-gpios" of IRQn */
    const struct oa_tc6_chip_ops* tc6_ops; /* oa_tc6 init step + vendor STATUS1 / IMASK1 */
    /* node id and MAC address are written by tc6_ops->chip_init, before
     * CONFIG0.SYNC (TC6 v1.1 7.6), also on oa_tc6_hw_reinit(); false: the
     * MAC driver writes them (and ZARFE) after oa_tc6_init_ops()
     */
    bool mac_in_chip_init;
    bool board_hat;        /* TSN-HAT FXL6408: PLCA LED, DIP-switch node id */
    bool runtime_frame_ts; /* oa_tc6_set_frame_ts() timestamps: TIMESTAMP=0 build only */
    int (*probe_cfg)(struct lan865x_priv* priv, struct spi_device* spi); /* SPI mode / clock, node id */
    int (*set_macaddr)(struct lan865x_priv* priv, const u8* mac);
    void (*set_rx_mode)(struct lan865x_priv* priv);
    int (*mac_enable)(struct lan865x_priv* priv, bool en);
    netdev_tx_t (*xmit)(struct lan865x_priv* priv, struct sk_buff* skb); /* TIMESTAMP=0 build */
    int (*hwtstamp_get)(struct lan865x_priv* priv, struct ifreq* ifr);   /* TIMESTAMP=0 build */
    int (*hwtstamp_set)(struct lan865x_priv* priv, struct ifreq* ifr);   /* TIMESTAMP=0 build */
    const struct ethtool_ops* ethtool_ops;
    int (*ptp_init)(struct lan865x_priv* priv); /* TIMESTAMP=0 build; failure is not fatal */
    void (*ptp_exit)(struct lan865x_priv* priv);
};

extern const struct lan865x_variant t1s_oaspi_variant;

#endif /* LAN865X_VARIANT_H */
