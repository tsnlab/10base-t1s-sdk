/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * OPEN Alliance 10BASE‑T1x MAC‑PHY Serial Interface framework
 *
 * Link: https://opensig.org/download/document/OPEN_Alliance_10BASET1x_MAC-PHY_Serial_Interface_V1.1.pdf
 *
 * Author: Parthiban Veerasooran <parthiban.veerasooran@microchip.com>
 */

#ifndef OA_TC6_H
#define OA_TC6_H

#include <linux/etherdevice.h>
#include <linux/spi/spi.h>

struct oa_tc6;

/* MAC-PHY specific part of the driver (vendor code lives in the MAC driver):
 * chip_init  - step of the init and the reinit sequence after the TC6 software
 *              reset and the error IRQ unmask, before CONFIG0.SYNC / the data
 *              transfer (NULL = none);
 * imask1_unmask / status1_handled / status1_fatal / status1_warn - bits of the
 *              vendor-specific OA_IMASK1 / OA_STATUS1 (TC6 v1.1 9.2.9 / 9.2.12):
 *              unmasked at init (with err_irq), cleared and counted on EXST,
 *              fatal (-ENODEV), logged.  0 = STATUS1 / IMASK1 left alone.
 */
struct oa_tc6_chip_ops {
    int (*chip_init)(struct oa_tc6* tc6);
    u32 imask1_unmask;
    u32 status1_handled;
    u32 status1_fatal;
    u32 status1_warn;
};

/* no MAC-PHY specific part */
struct oa_tc6* oa_tc6_init(struct spi_device* spi, struct net_device* netdev);
/* only the init step (no vendor STATUS1 / IMASK1 bits) */
struct oa_tc6* oa_tc6_init_chip(struct spi_device* spi, struct net_device* netdev,
                                int (*chip_init)(struct oa_tc6* tc6));
struct oa_tc6* oa_tc6_init_ops(struct spi_device* spi, struct net_device* netdev, const struct oa_tc6_chip_ops* ops);
void oa_tc6_exit(struct oa_tc6* tc6);
int oa_tc6_write_register(struct oa_tc6* tc6, u32 address, u32 value);
int oa_tc6_write_registers(struct oa_tc6* tc6, u32 address, u32 value[], u8 length);
int oa_tc6_read_register(struct oa_tc6* tc6, u32 address, u32* value);
int oa_tc6_read_registers(struct oa_tc6* tc6, u32 address, u32 value[], u8 length);
#ifdef FRAME_TIMESTAMP_ENABLE
netdev_tx_t oa_tc6_start_xmit(struct oa_tc6* tc6, struct sk_buff* skb, u8 ts_capture_mode);
#else  /* FRAME_TIMESTAMP_ENABLE */
netdev_tx_t oa_tc6_start_xmit(struct oa_tc6* tc6, struct sk_buff* skb);
/* Run-time frame timestamps (TC6 v1.1 7.8) for a MAC driver that wants them
 * in a TIMESTAMP=0 build (t1s-oaspi): TSC of one frame (0 none, 1..3 =
 * TTSCA..C, sent with SV = 1), and the receive timestamp in front of every
 * frame (rx_ts_len bytes, 0 = none; set from the chip_init hook).  rtsa = true
 * (STDCAP.FTSC = 1): the timestamp is there only when the footer of the start chunk
 * has RTSA = 1 and RTSP is checked (7.8.1); false (FTSC = 0): every frame has it.
 */
netdev_tx_t oa_tc6_start_xmit_tsc(struct oa_tc6* tc6, struct sk_buff* skb, u8 tsc);
void oa_tc6_set_frame_ts(struct oa_tc6* tc6, u8 rx_ts_len,
                         void (*rx_ts_cb)(void* ctx, struct sk_buff* skb, const u8* ts), void* ctx, bool rtsa);
#endif /* FRAME_TIMESTAMP_ENABLE */
int oa_tc6_zero_align_receive_frame_enable(struct oa_tc6* tc6);

/* MAC-PHY error/status event counters (OA_STATUS0/OA_STATUS1 bits seen
 * through the footer EXST path, plus footer errors).
 */
enum oa_tc6_stat_id {
    OA_TC6_STAT_TXPE,          /* STATUS0.0  transmit protocol error */
    OA_TC6_STAT_TXBOE,         /* STATUS0.1  transmit buffer overflow */
    OA_TC6_STAT_TXBUE,         /* STATUS0.2  transmit buffer underflow */
    OA_TC6_STAT_RXBOE,         /* STATUS0.3  receive buffer overflow */
    OA_TC6_STAT_LOFE,          /* STATUS0.4  loss of framing */
    OA_TC6_STAT_HDRE,          /* STATUS0.5  header (parity) error */
    OA_TC6_STAT_RESETC,        /* STATUS0.6  unexpected reset complete */
    OA_TC6_STAT_TXFCSE,        /* STATUS0.11 transmit FCS error */
    OA_TC6_STAT_CPDE,          /* STATUS0.12 control data protection error */
    OA_TC6_STAT_RXNER,         /* STATUS1.0  receive non-recoverable error */
    OA_TC6_STAT_TXNER,         /* STATUS1.1  transmit non-recoverable error */
    OA_TC6_STAT_FSMSTER,       /* STATUS1.17 internal FSM state error */
    OA_TC6_STAT_ECC,           /* STATUS1.18 SRAM ECC error */
    OA_TC6_STAT_UV18,          /* STATUS1.19 1.8 V under-voltage */
    OA_TC6_STAT_BUSER,         /* STATUS1.20 internal bus error */
    OA_TC6_STAT_FOOTER_HDRB,   /* footer HDRB: chip got a bad header */
    OA_TC6_STAT_FOOTER_UNSYNC, /* footer SYNC = 0: chip lost config */
    OA_TC6_STAT_FATAL,         /* fatal errors (traffic disabled) */
    OA_TC6_STAT_FOOTER_PARITY, /* receive footer with a parity error (MISO) */
    OA_TC6_STAT_RX_TS_PARITY,  /* receive timestamp (RTSP) parity error */
    OA_TC6_STAT_RX_TS_SPLIT,   /* frame dropped: receive timestamp split over two chunks */
    OA_TC6_STAT_NUM,
};

extern const char oa_tc6_stat_names[OA_TC6_STAT_NUM][32];
void oa_tc6_get_stats(struct oa_tc6* tc6, u64* data);

/* TX stall / fatal error recovery support */
struct oa_tc6_tx_state {
    bool tx_pending; /* a tx skb is waiting or partly sent */
    u16 tx_credits;  /* TXC from the last footer */
    bool disabled;   /* traffic disabled after a fatal error */
};

struct net_device* oa_tc6_get_netdev(struct oa_tc6* tc6);
void oa_tc6_set_fatal_cb(struct oa_tc6* tc6, void (*cb)(void* ctx), void* ctx);
void oa_tc6_get_tx_state(struct oa_tc6* tc6, struct oa_tc6_tx_state* st);
int oa_tc6_read_buffer_status(struct oa_tc6* tc6, u8* txc, u8* rba);
void oa_tc6_kick(struct oa_tc6* tc6);
void oa_tc6_stop_traffic(struct oa_tc6* tc6);
int oa_tc6_hw_reinit(struct oa_tc6* tc6);
void oa_tc6_start_traffic(struct oa_tc6* tc6);

#endif /* OA_TC6_H */
