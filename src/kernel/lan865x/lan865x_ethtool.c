// SPDX-License-Identifier: GPL-2.0+
/*
 * LAN865x ethtool operations
 *
 * Microchip's LAN865x 10BASE-T1S MAC-PHY driver
 */

#include <linux/ethtool.h>
#include <linux/netdevice.h>
#include <linux/net_tstamp.h>
#include <linux/phy.h>
#include <linux/ptp_clock_kernel.h>

#include "lan865x_arch.h"
#ifdef FRAME_TIMESTAMP_ENABLE
#include "lan865x_ptp.h"
#endif /* FRAME_TIMESTAMP_ENABLE */

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

const struct ethtool_ops lan865x_ethtool_ops = {
    .get_link_ksettings = phy_ethtool_get_link_ksettings,
    .set_link_ksettings = phy_ethtool_set_link_ksettings,
#ifdef FRAME_TIMESTAMP_ENABLE
    .get_ts_info = lan865x_ethtool_get_ts_info,
#endif /* FRAME_TIMESTAMP_ENABLE */
};
