/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * TSN Lab T1S MAC-PHY (OASPI + t1s_mac + 10BASE-T1S PHY) - the part of the
 * LAN865x driver that differs for this device.  Everything else (TC6 SPI
 * protocol, data path, IRQ, PHY over MMS 2..4, PLCA standard registers,
 * health / recovery, /dev/lan865xN) is the shared LAN865x code.
 *
 * Selected by DT compatible "tsnlab,t1s-oaspi" (LAN865X_CHIP_T1S).
 */
#ifndef T1S_OASPI_H
#define T1S_OASPI_H

#include <linux/types.h>

struct lan865x_priv;
struct oa_tc6;
struct spi_device;

/* priv->chip */
#define LAN865X_CHIP_LAN865X 0
#define LAN865X_CHIP_T1S 1

#define T1S_IS(priv) ((priv)->chip == LAN865X_CHIP_T1S)

/* MMS1 = t1s_mac register file (mac/rtl/t1s_mac_regs.v), 32-bit word
 * addresses 0x00..0xFF.  Only the registers the driver touches.
 */
#define T1S_MMS1_MAC_ID 0x00010000     /* RO 0x54314D02 */
#define T1S_MMS1_MAC_CTRL 0x00010001   /* .0 tx_en .1 rx_en .2 fdx .4 promisc .5 drop_echo */
#define T1S_MAC_CTRL_TXEN BIT(0)
#define T1S_MAC_CTRL_RXEN BIT(1)
#define T1S_MAC_CTRL_PROMISC BIT(4)
#define T1S_MMS1_MAC_ADDR_L 0x00010002 /* addr[31:0]; addr[47:40] = first octet on the wire */
#define T1S_MMS1_MAC_ADDR_H 0x00010003 /* .15:0 addr[47:32] */
#define T1S_MMS1_MCAST_HASH_L 0x00010005
#define T1S_MMS1_MCAST_HASH_H 0x00010006
#define T1S_MMS1_MAC_CAP 0x00010007
#define T1S_MMS1_TS_CFG 0x00010016 /* .7 FTSE .6 FTSS (not used: TIMESTAMP=0 build) */

#define T1S_DEF_SPI_HZ 12500000u
#define T1S_DEF_NODE_ID 1u

int t1s_oaspi_probe_cfg(struct lan865x_priv* priv, struct spi_device* spi);
int t1s_oaspi_chip_init(struct oa_tc6* tc6);
int t1s_oaspi_set_macaddr(struct lan865x_priv* priv, const u8* mac);
int t1s_oaspi_mac_enable(struct lan865x_priv* priv, bool en);
void t1s_oaspi_set_rx_mode(struct lan865x_priv* priv);
ssize_t t1s_oaspi_regdump(struct lan865x_priv* priv, char* buf);

#endif /* T1S_OASPI_H */
