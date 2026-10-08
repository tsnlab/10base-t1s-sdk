// SPDX-License-Identifier: GPL-2.0+
/*
 * LAN865x link health: MAC/PHY statistics, TX stall detection/recovery,
 * collision-detect policy.
 *
 * Copyright (c) 2026 TSN Lab
 */

#ifndef _LAN865X_HEALTH_H_
#define _LAN865X_HEALTH_H_

#include <linux/ethtool.h>
#include <linux/netdevice.h>

struct lan865x_priv;

/* Counters accumulated by the driver (u64, never cleared).  The hardware
 * ones are read-and-cleared from the chip by the monitor work; the chip
 * counters are 8 bit (saturating) or 32 bit, see DS60001734F 11.2.26-38.
 */
enum lan865x_hwstat_id {
	/* MMS1 STATS0..STATS12 */
	LAN865X_HW_RXSE,	/* STATS0[31:24] receive symbol errors */
	LAN865X_HW_LFER,	/* STATS0[23:16] length field errors */
	LAN865X_HW_OFRX,	/* STATS0[15:8]  oversize frames */
	LAN865X_HW_UFRX,	/* STATS0[7:0]   undersize frames */
	LAN865X_HW_RXRER,	/* STATS1[31:24] receive resource errors */
	LAN865X_HW_RXBOVR,	/* STATS1[23:16] receive buffer overruns */
	LAN865X_HW_RXFOVR,	/* STATS1[15:8]  receive FIFO overruns */
	LAN865X_HW_FCSE,	/* STATS2[7:0]   FCS errors */
	LAN865X_HW_UHMFRX,	/* STATS5[31:24] unicast hash matched */
	LAN865X_HW_MHMFRX,	/* STATS5[23:16] multicast hash matched */
	LAN865X_HW_BFRX,	/* STATS5[15:8]  broadcast frames */
	LAN865X_HW_VTRX,	/* STATS5[7:0]   VLAN tagged frames */
	LAN865X_HW_TFRX,	/* STATS6        total frames received */
	LAN865X_HW_FRX,		/* STATS7        frames received OK */
	LAN865X_HW_TXAIE,	/* STATS8[7:0]   TX abort, internal error */
	LAN865X_HW_TXAEE,	/* STATS9[31:24] TX abort, external (SPI host) error */
	LAN865X_HW_TXFUR,	/* STATS9[23:16] TX FIFO underruns */
	LAN865X_HW_TXBUR,	/* STATS9[15:8]  TX buffer underruns */
	LAN865X_HW_XCOL,	/* STATS10[7:0]  excessive collisions (16 attempts) */
	LAN865X_HW_TFTX,	/* STATS11       frames attempted (incl. errors) */
	LAN865X_HW_FTX,		/* STATS12       frames transmitted OK */
	/* MMS2 T1SPCSDIAG1/2 (802.3cg 3.2293/3.2294) */
	LAN865X_HW_RMTJAB,	/* T1SPCSDIAG1 remote jabber count */
	LAN865X_HW_CORTX,	/* T1SPCSDIAG2 corrupted transmit (physical collisions) */
	/* MMS4 STS1: sticky (clear-on-read) event bits, counted per poll */
	LAN865X_HW_STS1_PSTC,
	LAN865X_HW_STS1_TXCOL,
	LAN865X_HW_STS1_TXJAB,
	LAN865X_HW_STS1_EMPCYC,
	LAN865X_HW_STS1_RXINTO,
	LAN865X_HW_STS1_UNEXPB,
	LAN865X_HW_STS1_BCNBFTO,
	LAN865X_HW_STS1_UNCRS,
	LAN865X_HW_STS1_PLCASYM,
	LAN865X_HW_STS1_ESDERR,
	LAN865X_HW_STS1_DEC5B,
	/* driver */
	LAN865X_SW_SATURATED,	/* polls where an 8-bit chip counter was 0xFF */
	LAN865X_SW_POLL_ERRORS,	/* SPI errors while polling */
	/* TX stall detection / recovery */
	LAN865X_SW_STALL_DETECTED,	/* TXC 0 + no TX progress for tx_stall_ms */
	LAN865X_SW_REPLAY_DETECTED,	/* chip sent more frames than the host gave it */
	LAN865X_SW_WATCHDOG,	/* ndo_tx_timeout */
	LAN865X_SW_FATAL,	/* oa_tc6 fatal error */
	LAN865X_SW_KICKS,	/* credits back but no IRQ: data transfer re-run */
	LAN865X_SW_RCV_LIGHT_OK,	/* recovered by MAC_NCR.TXEN toggle */
	LAN865X_SW_RCV_RESET_OK,	/* recovered by MAC-PHY reset + reconfiguration */
	LAN865X_SW_RCV_FAILED,
	/* PTP TX timestamps */
	LAN865X_SW_PTP_TS_OK,
	LAN865X_SW_PTP_TS_MISSED,	/* OA_STATUS1.TTSCMx: capture missed */
	LAN865X_SW_PTP_TS_TIMEOUT,
	LAN865X_SW_PTP_TS_REPLACED,	/* new request before the old one completed */
	/* collision detect policy (CDCTL0.CDEN) */
	LAN865X_SW_CDEN_CHANGES,
	LAN865X_SW_CDEN,	/* gauge: current CDCTL0.CDEN */
	LAN865X_SW_PLCA_PST,	/* gauge: last PLCA_STS.PST */
	LAN865X_HWSTAT_NUM,
};

int lan865x_health_init(struct lan865x_priv* priv);
void lan865x_health_start(struct lan865x_priv* priv);
void lan865x_health_stop(struct lan865x_priv* priv);

void lan865x_get_stats64(struct net_device* netdev, struct rtnl_link_stats64* s);
int lan865x_get_sset_count(struct net_device* netdev, int sset);
void lan865x_get_strings(struct net_device* netdev, u32 sset, u8* data);
void lan865x_get_ethtool_stats(struct net_device* netdev, struct ethtool_stats* es, u64* data);

void lan865x_health_tx_timeout(struct net_device* netdev, unsigned int txqueue);

/* lan865x.c: re-apply MAC address, PLCA node ID, RX filters, TX/RX enable
 * after oa_tc6_hw_reinit()
 */
int lan865x_restore_hw_config(struct lan865x_priv* priv);

#endif /* _LAN865X_HEALTH_H_ */
