// SPDX-License-Identifier: GPL-2.0+
/*
 * Microchip's LAN865x 10BASE-T1S MAC-PHY driver (Standalone, no DT dependency)
 *
 * Original Author: Parthiban Veerasooran <parthiban.veerasooran@microchip.com>
 */

#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/phy.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/i2c.h>
#include <linux/err.h>

#include "lan865x_ioctl.h"
#include "lan865x_arch.h"
#include "lan865x_sysfs.h"
#include "t1s_hat_fxl6408.h"
#ifdef FRAME_TIMESTAMP_ENABLE
#include "lan865x_ptp.h"
#include <linux/net_tstamp.h>
#endif /* FRAME_TIMESTAMP_ENABLE */

#define DRV_NAME "lan8650"

/* --- MAC Registers --- */
#define LAN865X_REG_MAC_NET_CTL   0x00010000
#define MAC_NET_CTL_TXEN          BIT(3)
#define MAC_NET_CTL_RXEN          BIT(2)

#define LAN865X_REG_MAC_NET_CFG   0x00010001
#define MAC_NET_CFG_PROMISCUOUS_MODE  BIT(4)
#define MAC_NET_CFG_MULTICAST_MODE    BIT(6)
#define MAC_NET_CFG_UNICAST_MODE      BIT(7)

#define LAN865X_REG_MAC_L_HASH    0x00010020
#define LAN865X_REG_MAC_H_HASH    0x00010021
#define LAN865X_REG_MAC_L_SADDR1  0x00010022
#define LAN865X_REG_MAC_H_SADDR1  0x00010023

#define LAN865X_REG_PLCA_CTRL1    0x0004ca02

#define LAN8650_NODE_MAX_COUNT 8
#define NODE_ID_BITS_WIDTH 8
#define NODE_ID_MASK 0xFF
#define REGISTER_MAC_MASK 0xffffffff
#define MAC_ADDR_LENGTH 6
#define NUM_OF_BITS_IN_BYTE 8

static struct oa_tc6* g_tc6;

#define FXL6408_I2C_BUS   20
#define FXL6408_I2C_ADDR   0x43
#define NODE_ID_OFFSET 0x0F
#define NODE_ID_LEN 1

static int lan865x_plca_check_thread_handler(void* data) {
    struct lan865x_priv* priv = (struct lan865x_priv*)data;
    u32 status;

    while (!kthread_should_stop()) {
        /* Check every 200ms for PLCA status */
        msleep(200);

        if (!priv->fxl6408_initialized) {
            continue;
        }

        oa_tc6_read_register(priv->tc6, MMS4_PLCA_STS, &status);
        if (status & PLCA_BEACON_TX_RX_MASK) {
            t1s_hat_fxl6408_write_reg(priv, FXL6408_REG_OUTPUT_STATE, FXL6408_PLCA_OK_LED_ON);
        } else {
            t1s_hat_fxl6408_write_reg(priv, FXL6408_REG_OUTPUT_STATE, FXL6408_PLCA_OK_LED_OFF);
        }
    }
    return 0;
}

int lan865x_get_node_id(struct lan865x_priv *priv)
{
    pr_debug("lan865x: get_node_id=%d\n", priv->node_id);
    return priv->node_id;
}

int lan865x_set_nodeid(struct lan865x_priv *priv, u32 node_id)
{
    u32 regval = (LAN8650_NODE_MAX_COUNT << NODE_ID_BITS_WIDTH) |
                 (node_id & NODE_ID_MASK);

    pr_debug("lan865x: set_nodeid=%u (regval=0x%08x)\n", node_id, regval);
    return oa_tc6_write_register(priv->tc6, LAN865X_REG_PLCA_CTRL1, regval);
}

static int lan865x_set_hw_macaddr_low_bytes(struct oa_tc6 *tc6, const u8 *mac)
{
    u32 regval = (mac[3] << 24) | (mac[2] << 16) |
                 (mac[1] << 8) | mac[0];
    return oa_tc6_write_register(tc6, LAN865X_REG_MAC_L_SADDR1, regval);
}

static int lan865x_set_hw_macaddr(struct lan865x_priv *priv, const u8 *mac)
{
    int restore_ret;
    u32 regval;
    int ret;

    /* Configure MAC address low bytes */
    ret = lan865x_set_hw_macaddr_low_bytes(priv->tc6, mac);
    if (ret) {
        return ret;
    }

    /* Prepare and configure MAC address high bytes */
    regval = (mac[5] << 8) | mac[4];
    ret = oa_tc6_write_register(priv->tc6, LAN865X_REG_MAC_H_SADDR1, regval);
    if (!ret) {
        return 0;
    }

    /* Restore the old MAC address low bytes from netdev if the new MAC
     * address high bytes setting failed.
     */
    restore_ret = lan865x_set_hw_macaddr_low_bytes(priv->tc6, priv->netdev->dev_addr);
    if (restore_ret) {
        return restore_ret;
    }

    return ret;
}

#ifdef FRAME_TIMESTAMP_ENABLE
static int lan865x_ethtool_get_ts_info(struct net_device* netdev, struct ethtool_ts_info* ts_info) {
    struct lan865x_priv* priv = (struct lan865x_priv*)netdev_priv(netdev);

    if (!priv->ptpdev) {
        return -EOPNOTSUPP;
    }

    ts_info->phc_index = ptp_clock_index(priv->ptpdev->ptp_clock);

    ts_info->so_timestamping = SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE |
                               SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_RX_HARDWARE |
                               SOF_TIMESTAMPING_RAW_HARDWARE;

    ts_info->tx_types = BIT(HWTSTAMP_TX_OFF) | BIT(HWTSTAMP_TX_ON);

    ts_info->rx_filters = BIT(HWTSTAMP_FILTER_NONE) | BIT(HWTSTAMP_FILTER_ALL) | BIT(HWTSTAMP_FILTER_PTP_V2_L2_EVENT) |
                          BIT(HWTSTAMP_FILTER_PTP_V2_L2_SYNC) | BIT(HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ);

    return 0;
}
#else /* FRAME_TIMESTAMP_ENABLE */
static int lan865x_ethtool_get_ts_info(struct net_device* netdev, struct ethtool_ts_info* ts_info) {
    return -EOPNOTSUPP;
}
#endif /* FRAME_TIMESTAMP_ENABLE */

static const struct ethtool_ops lan865x_ethtool_ops = {
    .get_link_ksettings = phy_ethtool_get_link_ksettings,
    .set_link_ksettings = phy_ethtool_set_link_ksettings,
#ifdef FRAME_TIMESTAMP_ENABLE
    .get_ts_info = lan865x_ethtool_get_ts_info,
#endif /* FRAME_TIMESTAMP_ENABLE */
};


static int lan865x_set_mac_address(struct net_device *netdev, void *addr)
{
    struct lan865x_priv *priv = netdev_priv(netdev);
    struct sockaddr *address = addr;
    int ret;

    ret = eth_prepare_mac_addr_change(netdev, addr);
    if (ret < 0) {
        return ret;
    }

    if (ether_addr_equal(address->sa_data, netdev->dev_addr)) {
        return 0;
    }

    ret = lan865x_set_hw_macaddr(priv, address->sa_data);
    if (ret) {
        return ret;
    }

    eth_commit_mac_addr_change(netdev, addr);

    return 0;
}

/* ---------- Multicast / Hash Table ---------- */
static u32 get_address_bit(u8 addr[ETH_ALEN], u32 bit)
{
    return ((addr[bit / NUM_OF_BITS_IN_BYTE]) >> (bit % NUM_OF_BITS_IN_BYTE)) & 1;
}

static u32 lan865x_hash(u8 addr[ETH_ALEN])
{
    u32 hash_index = 0;

    for (int i = 0; i < MAC_ADDR_LENGTH; i++) {
        u32 hash = 0;

        for (int j = 0; j < NUM_OF_BITS_IN_BYTE; j++) {
            hash ^= get_address_bit(addr, (j * MAC_ADDR_LENGTH) + i);
        }

        hash_index |= (hash << i);
    }

    return hash_index;
}

static int lan865x_set_specific_multicast_addr(struct lan865x_priv *priv)
{
    struct netdev_hw_addr* hw_addr;
    u32 hash_lo = 0;
    u32 hash_hi = 0;
    int ret;

    netdev_for_each_mc_addr(hw_addr, priv->netdev) {
        u32 bit_num = lan865x_hash(hw_addr->addr);

        if (bit_num >= BIT(5)) {
            hash_hi |= (1 << (bit_num - BIT(5)));
        } else {
            hash_lo |= (1 << bit_num);
        }
    }

    /* Enabling specific multicast addresses */
    ret = oa_tc6_write_register(priv->tc6, LAN865X_REG_MAC_H_HASH, hash_hi);
    if (ret) {
        netdev_err(priv->netdev, "Failed to write reg_hashh: %d\n", ret);
        return ret;
    }

    ret = oa_tc6_write_register(priv->tc6, LAN865X_REG_MAC_L_HASH, hash_lo);
    if (ret) {
        netdev_err(priv->netdev, "Failed to write reg_hashl: %d\n", ret);
    }

    return ret;
}

static int lan865x_set_all_multicast_addr(struct lan865x_priv *priv)
{
    int ret;

    /* Enabling all multicast addresses */
    ret = oa_tc6_write_register(priv->tc6, LAN865X_REG_MAC_H_HASH, REGISTER_MAC_MASK);
    if (ret) {
        netdev_err(priv->netdev, "Failed to write reg_hashh: %d\n", ret);
        return ret;
    }

    ret = oa_tc6_write_register(priv->tc6, LAN865X_REG_MAC_L_HASH, REGISTER_MAC_MASK);
    if (ret) {
        netdev_err(priv->netdev, "Failed to write reg_hashl: %d\n", ret);
    }

    return ret;
}

static int lan865x_clear_all_multicast_addr(struct lan865x_priv *priv)
{
    int ret;

    ret = oa_tc6_write_register(priv->tc6, LAN865X_REG_MAC_H_HASH, 0);
    if (ret) {
        netdev_err(priv->netdev, "Failed to write reg_hashh: %d\n", ret);
        return ret;
    }

    ret = oa_tc6_write_register(priv->tc6, LAN865X_REG_MAC_L_HASH, 0);
    if (ret) {
        netdev_err(priv->netdev, "Failed to write reg_hashl: %d\n", ret);
    }

    return ret;
}

static void lan865x_multicast_work_handler(struct work_struct *work)
{
    struct lan865x_priv* priv = container_of(work, struct lan865x_priv, multicast_work);
    u32 regval = 0;
    int ret;

    if (priv->netdev->flags & IFF_PROMISC) {
        /* Enabling promiscuous mode */
        regval |= MAC_NET_CFG_PROMISCUOUS_MODE;
        regval &= (~MAC_NET_CFG_MULTICAST_MODE);
        regval &= (~MAC_NET_CFG_UNICAST_MODE);
    } else if (priv->netdev->flags & IFF_ALLMULTI) {
        /* Enabling all multicast mode */
        if (lan865x_set_all_multicast_addr(priv)) {
            return;
        }

        regval &= (~MAC_NET_CFG_PROMISCUOUS_MODE);
        regval |= MAC_NET_CFG_MULTICAST_MODE;
        regval &= (~MAC_NET_CFG_UNICAST_MODE);
    } else if (!netdev_mc_empty(priv->netdev)) {
        /* Enabling specific multicast mode */
        if (lan865x_set_specific_multicast_addr(priv)) {
            return;
        }

        regval &= (~MAC_NET_CFG_PROMISCUOUS_MODE);
        regval |= MAC_NET_CFG_MULTICAST_MODE;
        regval &= (~MAC_NET_CFG_UNICAST_MODE);
    } else {
        /* Enabling local mac address only */
        if (lan865x_clear_all_multicast_addr(priv)) {
            return;
        }
    }
    ret = oa_tc6_write_register(priv->tc6, LAN865X_REG_MAC_NET_CFG, regval);
    if (ret) {
        netdev_err(priv->netdev, "Failed to enable promiscuous/multicast/normal mode: %d\n", ret);
    }
}

static void lan865x_set_multicast_list(struct net_device *netdev)
{
    struct lan865x_priv* priv = netdev_priv(netdev);

    schedule_work(&priv->multicast_work);
}

/* ---------- Netdevice ---------- */
#ifdef FRAME_TIMESTAMP_ENABLE
static int lan865x_get_ts_config(struct net_device* netdev, struct ifreq* ifr) {
    struct lan865x_priv* priv = (struct lan865x_priv*)netdev_priv(netdev);
    struct hwtstamp_config* hwts_config = &priv->tstamp_config;

    return copy_to_user(ifr->ifr_data, hwts_config, sizeof(*hwts_config)) ? -EFAULT : 0;
}

static int lan865x_set_ts_config(struct net_device* netdev, struct ifreq* ifr) {
    struct lan865x_priv* priv = (struct lan865x_priv*)netdev_priv(netdev);
    struct hwtstamp_config* hwts_config = &priv->tstamp_config;

    return copy_from_user(hwts_config, ifr->ifr_data, sizeof(*hwts_config)) ? -EFAULT : 0;
}
#endif /* FRAME_TIMESTAMP_ENABLE */

static netdev_tx_t lan865x_send_packet(struct sk_buff *skb,
                                       struct net_device *netdev)
{
    struct lan865x_priv* priv = netdev_priv(netdev);
#ifdef FRAME_TIMESTAMP_ENABLE
    struct hwtstamp_config hwts_config = priv->tstamp_config;

    struct sk_buff* cloned_skb;
    u8 ts_capture_mode = LAN865X_TIMESTAMP_ID_NONE;

    if (skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP) {

        cloned_skb = skb_clone(skb, GFP_ATOMIC);
        if (!cloned_skb) {
            netdev_err(netdev, "%s: skb_clone() failed\n", __func__);
            return -EAGAIN;
        }

        /* NOTE:
         *	skb_clone() does not copy the user-space socket (sk) information.
         *	However, TX timestamping requires a valid sk to queue the timestamp to the user socket.
         *	Therefore, we manually copy the sk pointer from the original skb. */
        cloned_skb->sk = skb->sk;

        if (hwts_config.tx_type != HWTSTAMP_TX_ON) {
            ts_capture_mode = LAN865X_TIMESTAMP_ID_NONE;
            kfree_skb(cloned_skb);
        } else if (is_gptp_packet(skb)) {
            ts_capture_mode = LAN865X_TIMESTAMP_ID_GPTP;
            priv->waiting_txts_skb[LAN865X_TIMESTAMP_ID_GPTP] = skb_get(cloned_skb);
            skb_shinfo(priv->waiting_txts_skb[LAN865X_TIMESTAMP_ID_GPTP])->tx_flags |= SKBTX_IN_PROGRESS;
        } else {
            ts_capture_mode = LAN865X_TIMESTAMP_ID_NORMAL;
            priv->waiting_txts_skb[LAN865X_TIMESTAMP_ID_NORMAL] = skb_get(cloned_skb);
            skb_shinfo(priv->waiting_txts_skb[LAN865X_TIMESTAMP_ID_NORMAL])->tx_flags |= SKBTX_IN_PROGRESS;
        }
    }
    
    return oa_tc6_start_xmit(priv->tc6, skb, ts_capture_mode);
#else /* FRAME_TIMESTAMP_ENABLE */
    return oa_tc6_start_xmit(priv->tc6, skb);
#endif /* FRAME_TIMESTAMP_ENABLE */
}

static int lan865x_hw_disable(struct lan865x_priv* priv) {
    u32 regval;

    if (oa_tc6_read_register(priv->tc6, LAN865X_REG_MAC_NET_CTL, &regval)) {
        return -ENODEV;
    }

    regval &= ~(MAC_NET_CTL_TXEN | MAC_NET_CTL_RXEN);

    if (oa_tc6_write_register(priv->tc6, LAN865X_REG_MAC_NET_CTL, regval)) {
        return -ENODEV;
    }

    return 0;
}

static int lan865x_net_close(struct net_device *netdev)
{
    struct lan865x_priv* priv = netdev_priv(netdev);
    int ret;

    netif_stop_queue(netdev);
    phy_stop(netdev->phydev);
    ret = lan865x_hw_disable(priv);
    if (ret) {
        netdev_err(netdev, "Failed to disable the hardware: %d\n", ret);
        return ret;
    }

    return 0;
}

static int lan865x_hw_enable(struct lan865x_priv* priv) {
    u32 regval;

    if (oa_tc6_read_register(priv->tc6, LAN865X_REG_MAC_NET_CTL, &regval)) {
        return -ENODEV;
    }

    regval |= MAC_NET_CTL_TXEN | MAC_NET_CTL_RXEN;

    if (oa_tc6_write_register(priv->tc6, LAN865X_REG_MAC_NET_CTL, regval)) {
        return -ENODEV;
    }

    return 0;
}

static int lan865x_net_open(struct net_device *netdev)
{
    struct lan865x_priv* priv = netdev_priv(netdev);
    int ret;

    ret = lan865x_hw_enable(priv);
    if (ret) {
        netdev_err(netdev, "Failed to enable hardware: %d\n", ret);
        return ret;
    }

    phy_start(netdev->phydev);

    return 0;
}

static int lan865x_netdev_ioctl(struct net_device* netdev, struct ifreq* ifr, int cmd) {
#ifdef FRAME_TIMESTAMP_ENABLE
    switch (cmd) {
    case SIOCGHWTSTAMP:
        return lan865x_get_ts_config(netdev, ifr);
    case SIOCSHWTSTAMP:
        return lan865x_set_ts_config(netdev, ifr);
    default:
        return -EOPNOTSUPP;
    }
#else /* FRAME_TIMESTAMP_ENABLE */
    return -EOPNOTSUPP;
#endif /* FRAME_TIMESTAMP_ENABLE */
}

static const struct net_device_ops lan865x_netdev_ops = {
    .ndo_open = lan865x_net_open,
    .ndo_stop = lan865x_net_close,
    .ndo_start_xmit = lan865x_send_packet,
    .ndo_set_rx_mode = lan865x_set_multicast_list,
    .ndo_set_mac_address = lan865x_set_mac_address,
    .ndo_eth_ioctl = lan865x_netdev_ioctl,
};

static long lan865x_ioctl(struct file* file, unsigned int cmd, unsigned long arg);
static int lan865x_open(struct inode* inode, struct file* file);
static int lan865x_release(struct inode* inode, struct file* file);
static const struct file_operations lan865x_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = lan865x_ioctl,
    .open = lan865x_open,
    .release = lan865x_release,
};

static int lan865x_open(struct inode* inode, struct file* file) {
    struct spi_device* spi = container_of(file->private_data, struct spi_device, dev);
    file->private_data = spi;
    return 0;
}

static int lan865x_release(struct inode* inode, struct file* file) {
    file->private_data = NULL;
    return 0;
}

static void get_defined_mac(struct device *dev, struct lan865x_priv *priv, u8 *mac)
{
#ifdef USE_RANDOM_MAC
    eth_random_addr(mac);
    dev_info(dev, "Using runtime random MAC %pM\n", mac);

#elif defined(MACADDR)
    if (mac_pton(MACADDR, mac) && is_valid_ether_addr(mac)) {
        dev_info(dev, "Using build-time MAC %pM\n", mac);
    } else {
        eth_random_addr(mac);
        dev_warn(dev, "Invalid MACADDR provided, fallback to random %pM\n", mac);
    }

#else
    if (mac_pton("de:ad:be:ef:00:01", mac) && is_valid_ether_addr(mac)) {
        dev_info(dev, "Using default MAC %pM\n", mac);
    } else {
        eth_random_addr(mac);
        dev_warn(dev, "Default MAC invalid, fallback to random %pM\n", mac);
    }
#endif

    /* Read input status register */
    u8 val = t1s_hat_fxl6408_read_reg(priv, FXL6408_REG_INPUT_STATUS);
    u8 node_id = (val & 0xF0) >> 4;
    priv->node_id = node_id;
    lan865x_set_nodeid(priv, node_id);

#ifdef ENABLE_DIPSWITCH
    {
        if (node_id >= 0) {
            mac[5] = (mac[5] & 0xF0) | (node_id & 0x0F);
            dev_info(dev, "MAC updated with NodeID=%d -> %pM\n", node_id, mac);
        } else {
            dev_warn(dev, "NodeID not available, using base MAC %pM\n", mac);
        }
    }
#endif
}

/* ---------- SPI Driver ---------- */
static int lan865x_probe(struct spi_device *spi)
{
    struct net_device *netdev;
    struct lan865x_priv *priv;
    struct gpio_desc* lan8651_phy_irq_gpiod;
    int lan8651_phy_irq;
    int ret;

#ifdef CONFIG_RPI4
    (void)lan8651_phy_irq_gpiod;

    /* Get lan8651 phy irq from platform device*/
    lan8651_phy_irq = platform_get_irq(to_platform_device(&spi->dev), 0 /* TODO Macro */);
    if (lan8651_phy_irq < 0) {
        dev_err(&spi->dev, "Failed to get IRQ: %d\n", lan8651_phy_irq);
        return -EFAULT;
    }
#elif defined(CONFIG_RPI5)
    /* Get lan8651 phy irq from device tree*/
    lan8651_phy_irq_gpiod = devm_gpiod_get(&spi->dev, "lan8651irq", GPIOD_IN);
    if (IS_ERR(lan8651_phy_irq_gpiod)) {
        ret = PTR_ERR(lan8651_phy_irq_gpiod);
        dev_err(&spi->dev, "Failed to get IRQ: %d\n", ret);
        return -EFAULT;
    }
    lan8651_phy_irq = gpiod_to_irq(lan8651_phy_irq_gpiod);
    if (lan8651_phy_irq < 0) {
        dev_err(&spi->dev, "Failed to get IRQ: %d\n", lan8651_phy_irq);
        return -EFAULT;
    }
#endif

    /* Set spi mode */
    spi->mode = SPI_MODE_3;
    spi->mode &= ~SPI_CS_HIGH;
    spi->bits_per_word = 8;
    spi->max_speed_hz = 25000000;
    spi->irq = lan8651_phy_irq;

    netdev = alloc_etherdev(sizeof(*priv));
    if (!netdev)
        return -ENOMEM;

    priv = netdev_priv(netdev);
    priv->netdev = netdev;
    priv->spi = spi;

    /* Initialize oa_tc6 and lan8651*/
    priv->tc6 = oa_tc6_init(spi, netdev);
    g_tc6 = priv->tc6;
    if (!priv->tc6) {
        dev_err(&spi->dev, "oa_tc6_init failed\n");
        goto oa_tc6_init_error;
    }

    spi_set_drvdata(spi, priv);
    INIT_WORK(&priv->multicast_work, lan865x_multicast_work_handler);

#if 0 // Jihoon
    #define LAN8650_REG_MMS00_OA_ID     0x00000000
    #define LAN8650_REG_MMS00_OA_PHYID  0x00000001
    #define LAN8650_REG_MMS10_DEVID     0x000A0094

	u32 regval;

	oa_tc6_read_register(priv->tc6, LAN8650_REG_MMS00_OA_ID, &regval);
	pr_info("lan865x: OA_ID=0x%08x\n", regval);

	//oa_tc6_read_register(priv->tc6, LAN8650_REG_MMS00_OA_PHYID, &regval);
	//pr_info("lan865x: OA_PHYID=0x%08x\n", regval);

	oa_tc6_read_register(priv->tc6, LAN8650_REG_MMS10_DEVID, &regval);
	pr_info("lan865x: DEVID=0x%08x\n", regval);
#endif

#if 1 /* Jihoon */

    /* Create sysfs device */
    ret = lan865x_sysfs_create_device(priv);
    if (ret) {
        dev_err(&spi->dev, "Failed to create sysfs device: %d\n", ret);
        goto sysfs_create_error;
    }
    ret = t1s_hat_fxl6408_init(priv, &spi->dev);
    if (ret) {
        dev_err(&spi->dev, "Failed to initialize node ID updater: %d\n", ret);
        goto t1s_hat_fxl6408_init_error;
    }

    priv->plca_check_thread = kthread_run(lan865x_plca_check_thread_handler, priv, "lan865x-plca-check-thread");
    if (IS_ERR(priv->plca_check_thread)) {
        dev_err(&spi->dev, "Failed to create PLCA status check thread: %ld\n", PTR_ERR(priv->plca_check_thread));
        goto t1s_hat_fxl6408_init_error;
    }

    ret = oa_tc6_zero_align_receive_frame_enable(priv->tc6);
    if (ret) {
        dev_err(&spi->dev, "Failed to set ZARFE: %d\n", ret);
        goto zero_align_receive_frame_enable_error;
    }

    u8 mac[ETH_ALEN];
    get_defined_mac(&spi->dev, priv, mac);
    eth_hw_addr_set(netdev, mac);
    lan865x_set_hw_macaddr(priv, mac);

    netdev->if_port = IF_PORT_10BASET;
    netdev->irq = spi->irq;
    netdev->netdev_ops = &lan865x_netdev_ops;
    netdev->ethtool_ops = &lan865x_ethtool_ops;

    ret = register_netdev(netdev);
    if (ret) {
        dev_err(&spi->dev, "register_netdev failed: %d\n", ret);
        goto register_netdev_error;
    }

#ifdef FRAME_TIMESTAMP_ENABLE
    priv->ptpdev = ptp_device_init(&spi->dev, priv->tc6, (s32)spi->max_speed_hz);
    if (!priv->ptpdev) {
        dev_err(&spi->dev, "ptp_device_init() failed\n");
        goto ptp_device_init_error;
    }
#else /* FRAME_TIMESTAMP_ENABLE */
    priv->ptpdev = NULL;
#endif /* FRAME_TIMESTAMP_ENABLE */
    dev_info(&spi->dev, "LAN865x registered with MAC %pM\n", netdev->dev_addr);
    
    return 0;

#else /* Jihoon */
    ret = t1s_hat_fxl6408_init(priv, &spi->dev);
    if (ret) {
        dev_err(&spi->dev, "Failed to initialize node ID updater: %d\n", ret);
        goto t1s_hat_fxl6408_init_error;
    }

    u8 val = t1s_hat_fxl6408_read_reg(priv, FXL6408_REG_INPUT_STATUS);
    u8 node_id = (val & 0xF0) >> 4;
    priv->node_id = node_id;
    dev_info(&spi->dev, "node_id: %d\n", node_id);

    return 0;
#endif /* Jihoon */

ptp_device_init_error:
    unregister_netdev(netdev);
register_netdev_error:
zero_align_receive_frame_enable_error:
t1s_hat_fxl6408_init_error:
    t1s_hat_fxl6408_exit(priv);
sysfs_create_error:
    lan865x_sysfs_remove_device(priv);
oa_tc6_init_error:
    g_tc6 = NULL;
    oa_tc6_exit(priv->tc6);
    free_netdev(netdev);
    return -ENODEV;
}

static void lan865x_remove(struct spi_device *spi)
{
    struct lan865x_priv *priv = spi_get_drvdata(spi);
    struct net_device *netdev = priv ? priv->netdev : NULL;

    dev_info(&spi->dev, "lan865x: remove start\n");

    if (!priv) {
        dev_warn(&spi->dev, "lan865x: priv is NULL in remove\n");
        return;
    }
    if (!netdev) {
        dev_warn(&spi->dev, "lan865x: netdev is NULL in remove\n");
        return;
    }

    kthread_stop(priv->plca_check_thread);
    cancel_work_sync(&priv->multicast_work);

    /* Remove sysfs device */
    lan865x_sysfs_remove_device(priv);
    t1s_hat_fxl6408_exit(priv);

    dev_info(&spi->dev, "lan865x: unregistering netdev %s\n", netdev_name(netdev));
    dev_info(&spi->dev, "lan865x: MAC before unregister %pM\n", netdev->dev_addr);

    unregister_netdev(netdev);

    dev_info(&spi->dev, "lan865x: MAC after unregister %pM\n", netdev->dev_addr);

#ifdef FRAME_TIMESTAMP_ENABLE
    if (priv->ptpdev) {
        dev_info(&spi->dev, "lan865x: destroying PTP device\n");
        ptp_device_destroy(priv->ptpdev);
        priv->ptpdev = NULL;
    }
#endif /* FRAME_TIMESTAMP_ENABLE */

    if (priv->tc6) {
        dev_info(&spi->dev, "lan865x: calling oa_tc6_exit\n");
        g_tc6 = NULL;
        oa_tc6_exit(priv->tc6);
    }

    dev_info(&spi->dev, "lan865x: calling free_netdev\n");
    dev_info(&spi->dev, "lan865x: MAC before free %pM\n", netdev->dev_addr);

    free_netdev(netdev);

    dev_info(&spi->dev, "lan865x: remove finished\n");
}

static long lan865x_ioctl(struct file* file, unsigned int cmd, unsigned long arg) {
    struct lan865x_reg reg;
    int ret = 0;

    switch (cmd) {
    case LAN865X_READ_REG:
        if (copy_from_user(&reg, (void __user*)arg, sizeof(reg))) {
            return -EFAULT;
        }

        ret = oa_tc6_read_register(g_tc6, reg.addr, &reg.value);
        if (ret < 0) {
            return -ENODEV;
        }

        if (copy_to_user((void __user*)arg, &reg, sizeof(reg))) {
            return -EFAULT;
        }
        break;

    case LAN865X_WRITE_REG:
        if (copy_from_user(&reg, (void __user*)arg, sizeof(reg))) {
            return -EFAULT;
        }

        ret = oa_tc6_write_register(g_tc6, reg.addr, reg.value);
        break;

    default:
        return -ENOTTY;
    }

    return ret;
}

/* ---------- SPI/Module Tables ---------- */
static const struct spi_device_id lan865x_ids[] = {
    { "lan8650", 0 },
    { "lan8651", 0 },
    { "spidev",  0 },
    { },
};
MODULE_DEVICE_TABLE(spi, lan865x_ids);

static const struct of_device_id lan865x_of_match[] = {
    { .compatible = "microchip,lan8650" },
    { .compatible = "microchip,lan8651" },
    { }
};
MODULE_DEVICE_TABLE(of, lan865x_of_match);

static struct spi_driver lan865x_driver = {
    .driver = {
        .name = DRV_NAME,
        .of_match_table = lan865x_of_match,
    },
    .id_table = lan865x_ids,
    .probe = lan865x_probe,
    .remove = lan865x_remove,
};

static int __init lan865x_init(void)
{
    int ret;

    /* Initialize sysfs */
    ret = lan865x_sysfs_init();
    if (ret) {
        pr_err("lan865x: failed to initialize sysfs: %d\n", ret);
        return ret;
    }

    /* Register SPI driver */
    return spi_register_driver(&lan865x_driver);
}

static void __exit lan865x_exit(void)
{
    spi_unregister_driver(&lan865x_driver);
    lan865x_sysfs_exit();
}

module_init(lan865x_init);
module_exit(lan865x_exit);

MODULE_DESCRIPTION("LAN865x 10Base-T1S MACPHY Ethernet Driver (Standalone)");
MODULE_AUTHOR("Microchip / sbcho integration");
MODULE_LICENSE("GPL");
