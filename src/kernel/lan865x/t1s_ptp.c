// SPDX-License-Identifier: GPL-2.0+
/*
 * TSN Lab T1S MAC-PHY: PTP hardware clock and frame timestamps for the
 * t1s-oaspi path (TIMESTAMP=0 build; the LAN865x PTP code in lan865x_ptp.c
 * is not used for this device).
 *
 * Hardware (t1s repo mac/rtl/t1s_mac_regs.v, mac/rtl/t1s_mac_tsu.v,
 * docs/notes/54_tc6ts.md): the clock is in the MAC, MMS 1, command model.
 *   0x10..0x15 TTSCAH..TTSCCL  TX capture A..C, 64-bit format
 *                              {sec[31:0]} / {00, ns[29:0]} (FTSS = 1)
 *   0x16 TS_CFG   .7 FTSE .6 FTSS  (TC6 CONFIG0 bit positions)
 *   0x17 TS_STS   .8..10 TTSCAA..AC, .16..18 LOST A..C, .20..22 OVW A..C,
 *                 .24 PPS, .25 CMDERR, .26 WERR  (W1C)
 *   0x19 / 0x1A   TS_TX_LAT / TS_RX_LAT, signed 24-bit ns, reset 0
 *   0x20 PTP_CMD  write .2:0 = 1 LOAD 2 ADJ 3 SNAP 4 INC 5 PPS_ARM 6 PPS_STOP;
 *                 read .8 BUSY .9 PPS_ARMED .10 PPS level
 *   0x21 / 0x22   SET_SEC / SET_NS (LOAD), 0x23 ADJ (signed ns, |x| < 1e9)
 *   0x24 PTP_INC  ns per clk_s cycle, Q8.24 (reset 0x14000000 = 20 ns)
 *   0x25 / 0x26   SNAP_SEC / SNAP_NS
 *   0x27 / 0x28   PPS_START_SEC / PPS_START_NS, 0x29 PPS_PERIOD, 0x2A PPS_WIDTH
 *                 (ns; reset 1e9 / 1e8)
 * Writes to 0x20..0x24 and 0x27..0x2A while BUSY are ignored (WERR), so every
 * command waits for BUSY to clear before the next register write.
 *
 * Periodic output (notes/54 3.5, vendor - TC6 has no PPS): PPS_ARM raises the
 * MAC's pps output when the clock reaches START, lowers it WIDTH ns later and
 * raises it again every PERIOD ns (edges on the clk_s grid, 0..20 ns after the
 * target).  1 <= PERIOD <= 1e9, 0 < WIDTH < PERIOD, START_NS < 1e9 (else
 * CMDERR, nothing sent); START must be in the future (else not armed, CMDERR).
 * LOAD and ADJ (time steps) disarm it; INC (rate) does not.  On the TSN-HAT R2
 * board bits the output is FPGA pin AB16 (net TSN_PPS -> R11 -> J5 pin 1).
 *
 * Receive (TC6 v1.1 7.8.1): with TS_CFG = FTSE | FTSS the MAC puts the
 * 8-byte timestamp (SFD on the MII receive side, clock of this MAC) in front
 * of every frame; oa_tc6 strips it (oa_tc6_set_frame_ts()).  A MAC-PHY with
 * STDCAP.FTSC = 1 (this chip since TSCEN) reports it in the footer of the start
 * chunk (RTSA / RTSP, 7.8.1) and oa_tc6 takes the decision from there; with
 * FTSC = 0 (an older bitstream) the footer says nothing and the driver knows the
 * layout from TS_CFG.
 *
 * Transmit (TC6 v1.1 7.8.2): TSC in the header of the first chunk of the
 * frame, capture register = slot 1..3, completion polled in TS_STS.  The
 * OASPI passes the TSC to the MAC only when STDCAP.FTSC = 1 (this chip since
 * TSCEN, t1s repo top/rtl/chip/spi/t1s_chip_spi_top.v CoreFtsc = 1; an older
 * bitstream ties tx_tsc_i to 0, OASPI decision OD-8): TX hardware timestamps are
 * only offered when STDCAP.FTSC reads 1, or for a test with t1s_ts_tx_force=1
 * (requests then time out).
 *
 * Reference plane (802.1AS-2020 8.4.3): the TX event is the PHY's SFD at the
 * PCS input (below the PLCA delay line), the RX event the MII SFD after the
 * PHY's constant 760 ns receive shift; the MDI is not the reference.  The
 * constant parts are left to the host (TS_TX_LAT / TS_RX_LAT reset 0): either
 * t1s_ts_tx_lat_ns / t1s_ts_rx_lat_ns here (written to the MAC) or ptp4l
 * egressLatency / ingressLatency (t1s repo docs/notes/88).
 */
#include "t1s_oaspi.h"

#ifdef FRAME_TIMESTAMP_ENABLE
/* TIMESTAMP=1 build: the t1s probe refuses to run (lan865x.c), only what
 * t1s_oaspi.c links against
 */
int t1s_ptp_chip_init(struct oa_tc6* tc6, struct lan865x_priv* priv) {
    return 0;
}

ssize_t t1s_ptp_stats(struct lan865x_priv* priv, char* buf, ssize_t n) {
    return n;
}
#else /* FRAME_TIMESTAMP_ENABLE */

#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/net_tstamp.h>
#include <linux/netdevice.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/version.h>
#include <linux/wait.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

#include "lan865x_arch.h"

#define T1S_MMS1(a) (0x00010000u | (a))
#define T1S_TTSC_H(slot) T1S_MMS1(0x10 + 2 * ((slot) - 1))
#define T1S_TTSC_L(slot) T1S_MMS1(0x11 + 2 * ((slot) - 1))
#define T1S_TS_CFG_FTSE BIT(7)
#define T1S_TS_CFG_FTSS BIT(6)
#define T1S_TS_STS T1S_MMS1(0x17)
#define T1S_TS_STS_AVAIL(s) BIT(7 + (s))  /* .8 .9 .10 */
#define T1S_TS_STS_LOST(s) BIT(15 + (s))  /* .16 .17 .18 */
#define T1S_TS_STS_OVW(s) BIT(19 + (s))   /* .20 .21 .22 */
#define T1S_TS_STS_SLOT(s) (T1S_TS_STS_AVAIL(s) | T1S_TS_STS_LOST(s) | T1S_TS_STS_OVW(s))
#define T1S_TS_STS_CMDERR BIT(25)
#define T1S_TS_STS_WERR BIT(26)
#define T1S_TS_TX_LAT T1S_MMS1(0x19)
#define T1S_TS_RX_LAT T1S_MMS1(0x1A)
#define T1S_PTP_CMD T1S_MMS1(0x20)
#define T1S_PTP_CMD_BUSY BIT(8)
#define T1S_PTP_CMD_ARMED BIT(9)
#define T1S_PTP_SET_SEC T1S_MMS1(0x21)
#define T1S_PTP_SET_NS T1S_MMS1(0x22)
#define T1S_PTP_ADJ T1S_MMS1(0x23)
#define T1S_PTP_INC T1S_MMS1(0x24)
#define T1S_PTP_SNAP_SEC T1S_MMS1(0x25)
#define T1S_PTP_SNAP_NS T1S_MMS1(0x26)
#define T1S_PPS_START_SEC T1S_MMS1(0x27)
#define T1S_PPS_START_NS T1S_MMS1(0x28)
#define T1S_PPS_PERIOD T1S_MMS1(0x29)
#define T1S_PPS_WIDTH T1S_MMS1(0x2A)
#define T1S_CMD_LOAD 1
#define T1S_CMD_ADJ 2
#define T1S_CMD_SNAP 3
#define T1S_CMD_INC 4
#define T1S_CMD_PPS_ARM 5
#define T1S_CMD_PPS_STOP 6
/* periodic output: high and low parts of at least one clk_s cycle (the pin
 * moves on the 20 ns grid), default width min(period / 2, 100 ms) (100 ms =
 * the MAC's reset PPS_WIDTH), first edge at least 10 ms after the clock was
 * read (the arm sequence is six SPI register accesses)
 */
#define T1S_PO_MIN_NS 20u
#define T1S_PO_WIDTH_DEF_MAX 100000000u
#define T1S_PO_MARGIN_NS 10000000u
#define T1S_PO_TRIES 3
#define T1S_INC_NOMINAL 0x14000000u /* 20.0 ns: clk_s 50 MHz */
#define T1S_NS_MASK 0x3FFFFFFFu
#define T1S_TC6_STDCAP 0x00000002
#define T1S_TC6_STDCAP_FTSC BIT(6) /* TC6 v1.1 Table 10 */
#define T1S_NSLOT 3

static bool t1s_ts = true;
module_param(t1s_ts, bool, 0444);
MODULE_PARM_DESC(t1s_ts, "t1s-oaspi: PTP clock + receive timestamps (MAC TS_CFG = FTSE|FTSS, 8 bytes in front of each frame); 0 = off");

static bool t1s_ts_tx_force;
module_param(t1s_ts_tx_force, bool, 0444);
MODULE_PARM_DESC(t1s_ts_tx_force, "t1s-oaspi: offer TX hardware timestamps even if STDCAP.FTSC = 0 (test only: the requests time out)");

static int t1s_ts_tx_lat_ns;
module_param(t1s_ts_tx_lat_ns, int, 0444);
MODULE_PARM_DESC(t1s_ts_tx_lat_ns, "t1s-oaspi: TS_TX_LAT (egress latency added by the MAC, ns, default 0)");

static int t1s_ts_rx_lat_ns;
module_param(t1s_ts_rx_lat_ns, int, 0444);
MODULE_PARM_DESC(t1s_ts_rx_lat_ns, "t1s-oaspi: TS_RX_LAT (ingress latency subtracted by the MAC, ns, default 0)");

static unsigned int t1s_ts_poll_us = 200;
module_param(t1s_ts_poll_us, uint, 0644);
MODULE_PARM_DESC(t1s_ts_poll_us, "t1s-oaspi: TS_STS poll interval while a TX timestamp is outstanding (us)");

static unsigned int t1s_ts_timeout_ms = 100;
module_param(t1s_ts_timeout_ms, uint, 0644);
MODULE_PARM_DESC(t1s_ts_timeout_ms, "t1s-oaspi: give up on a TX timestamp after this many ms");

struct t1s_ptp {
    struct lan865x_priv* priv;
    struct ptp_clock* clock;
    struct ptp_clock_info info;
    struct mutex cmd_lock; /* PTP_CMD sequences */
    u32 inc;               /* PTP_INC now; written again after a MAC-PHY reset */
    bool tx_cap;           /* TX timestamps offered (STDCAP.FTSC or forced) */

    spinlock_t slot_lock;
    struct sk_buff* slot_skb[T1S_NSLOT + 1]; /* [1..3] = TTSCA..C */
    unsigned long slot_start[T1S_NSLOT + 1];
    u8 next_slot;
    wait_queue_head_t wq;
    struct task_struct* thread;

    u64 tx_ok, tx_lost, tx_timeout, tx_noslot;

    /* periodic output 0 = the MAC's pps pin; under cmd_lock */
    bool po_on;     /* requested (PTP_CLK_REQ_PEROUT on, armed at least once) */
    u64 po_start;   /* phase reference, PHC ns: edges at po_start + k * po_period */
    u32 po_period;  /* ns, <= 1e9 */
    u32 po_width;   /* ns */
    u64 po_next;    /* first rising edge of the last arm, PHC ns */
    u64 po_arms, po_rearms, po_errs;
};

static inline ktime_t t1s_ts64(u32 sec, u32 lo) {
    return ktime_set(sec, lo & T1S_NS_MASK);
}

/* ---- receive (oa_tc6 rx_ts_cb, SPI IRQ thread) ---- */

static void t1s_ptp_rx_ts(void* ctx, struct sk_buff* skb, const u8* ts) {
    struct lan865x_priv* priv = ctx;

    if (READ_ONCE(priv->tstamp_config.rx_filter) == HWTSTAMP_FILTER_NONE) {
        return;
    }
    skb_hwtstamps(skb)->hwtstamp = t1s_ts64(get_unaligned_be32(ts), get_unaligned_be32(ts + 4));
}

/* ---- clock commands ---- */

static int t1s_wait_idle(struct oa_tc6* tc6) {
    u32 v;
    int ret;

    for (int i = 0; i < 50; i++) {
        ret = oa_tc6_read_register(tc6, T1S_PTP_CMD, &v);
        if (ret) {
            return ret;
        }
        if (!(v & T1S_PTP_CMD_BUSY)) {
            return 0;
        }
        usleep_range(20, 40);
    }
    return -ETIMEDOUT;
}

/* write the command, wait for BUSY to drop, check CMDERR / WERR */
static int t1s_cmd(struct oa_tc6* tc6, u32 op) {
    u32 sts;
    int ret;

    ret = oa_tc6_write_register(tc6, T1S_PTP_CMD, op);
    if (!ret) {
        ret = t1s_wait_idle(tc6);
    }
    if (!ret) {
        ret = oa_tc6_read_register(tc6, T1S_TS_STS, &sts);
    }
    if (!ret && (sts & (T1S_TS_STS_CMDERR | T1S_TS_STS_WERR))) {
        oa_tc6_write_register(tc6, T1S_TS_STS, sts & (T1S_TS_STS_CMDERR | T1S_TS_STS_WERR));
        ret = -EIO;
    }
    return ret;
}

static int t1s_set_inc(struct oa_tc6* tc6, u32 inc) {
    int ret = oa_tc6_write_register(tc6, T1S_PTP_INC, inc);

    return ret ? ret : t1s_cmd(tc6, T1S_CMD_INC);
}

static int t1s_gettime(struct t1s_ptp* p, struct timespec64* ts, struct ptp_system_timestamp* sts) {
    struct oa_tc6* tc6 = p->priv->tc6;
    u32 sec, ns;
    int ret;

    ptp_read_system_prets(sts);
    ret = oa_tc6_write_register(tc6, T1S_PTP_CMD, T1S_CMD_SNAP);
    ptp_read_system_postts(sts);
    if (!ret) {
        ret = t1s_wait_idle(tc6);
    }
    if (!ret) {
        ret = oa_tc6_read_register(tc6, T1S_PTP_SNAP_SEC, &sec);
    }
    if (!ret) {
        ret = oa_tc6_read_register(tc6, T1S_PTP_SNAP_NS, &ns);
    }
    if (ret) {
        return ret;
    }
    ts->tv_sec = sec;
    ts->tv_nsec = ns & T1S_NS_MASK;
    return 0;
}

static int t1s_settime(struct t1s_ptp* p, const struct timespec64* ts) {
    struct oa_tc6* tc6 = p->priv->tc6;
    int ret;

    if (ts->tv_sec < 0 || ts->tv_sec > U32_MAX) {
        return -ERANGE; /* 32-bit seconds (TC6 64-bit format, notes/54 D4) */
    }
    ret = oa_tc6_write_register(tc6, T1S_PTP_SET_SEC, (u32)ts->tv_sec);
    if (!ret) {
        ret = oa_tc6_write_register(tc6, T1S_PTP_SET_NS, (u32)ts->tv_nsec);
    }
    return ret ? ret : t1s_cmd(tc6, T1S_CMD_LOAD);
}

/* ---- periodic output (PTP_CLK_REQ_PEROUT 0 = the MAC's pps pin) ---- */

/* Arm the MAC PPS at the first edge po_start + k * po_period that lies at
 * least T1S_PO_MARGIN_NS after the clock now.  The MAC refuses a START that
 * has passed when the command arrives (CMDERR, not armed); then the clock is
 * read again and a later edge taken.  cmd_lock held.
 */
static int t1s_po_arm(struct t1s_ptp* p) {
    struct oa_tc6* tc6 = p->priv->tc6;
    struct timespec64 now;
    u64 t = 0, min;
    u32 ns = 0, v = 0;
    int ret = -EIO;

    for (int i = 0; i < T1S_PO_TRIES; i++) {
        ret = t1s_gettime(p, &now, NULL);
        if (ret) {
            return ret;
        }
        min = (u64)timespec64_to_ns(&now) + ((u64)T1S_PO_MARGIN_NS << i);
        t = p->po_start;
        if (t < min) {
            t += div_u64(min - t + p->po_period - 1, p->po_period) * p->po_period;
        }
        if (div_u64(t, NSEC_PER_SEC) > U32_MAX) {
            return -ERANGE; /* 32-bit seconds (notes/54 D4) */
        }
        /* old CMDERR / WERR (W1C), so the status below answers this ARM */
        ret = oa_tc6_write_register(tc6, T1S_TS_STS, T1S_TS_STS_CMDERR | T1S_TS_STS_WERR);
        if (!ret) {
            ret = oa_tc6_write_register(tc6, T1S_PPS_START_SEC, (u32)div_u64_rem(t, NSEC_PER_SEC, &ns));
        }
        if (!ret) {
            ret = oa_tc6_write_register(tc6, T1S_PPS_START_NS, ns);
        }
        if (!ret) {
            ret = oa_tc6_write_register(tc6, T1S_PPS_PERIOD, p->po_period);
        }
        if (!ret) {
            ret = oa_tc6_write_register(tc6, T1S_PPS_WIDTH, p->po_width);
        }
        if (!ret) {
            ret = t1s_cmd(tc6, T1S_CMD_PPS_ARM);
        }
        if (ret != -EIO) {
            break; /* -EIO = CMDERR: START passed while the arguments were written */
        }
    }
    if (!ret) {
        ret = oa_tc6_read_register(tc6, T1S_PTP_CMD, &v);
    }
    if (!ret && !(v & T1S_PTP_CMD_ARMED)) {
        ret = -EIO;
    }
    if (!ret) {
        p->po_next = t;
    }
    return ret;
}

/* LOAD and ADJ disarm the MAC PPS (notes/54 3.5, so the pin does not chase a
 * stepped clock): arm it again on the same phase, at the next edge after the
 * step (at most one pulse is lost; a pulse that is high during the step is cut
 * short).  Only when the hardware is not armed: a step that failed may not
 * have reached the clock.  The step's own result is not changed.  cmd_lock
 * held.
 */
static void t1s_po_after_step(struct t1s_ptp* p) {
    u32 v;
    int ret;

    if (!p->po_on) {
        return;
    }
    if (!oa_tc6_read_register(p->priv->tc6, T1S_PTP_CMD, &v) && (v & T1S_PTP_CMD_ARMED)) {
        return;
    }
    ret = t1s_po_arm(p);
    if (ret) {
        p->po_errs++; /* the request stands: the next step tries again */
        dev_warn_ratelimited(&p->priv->spi->dev, "t1s-oaspi: periodic output not re-armed after a clock step: %d\n",
                             ret);
        return;
    }
    p->po_rearms++;
}

static int t1s_ptp_gettimex64(struct ptp_clock_info* info, struct timespec64* ts, struct ptp_system_timestamp* sts) {
    struct t1s_ptp* p = container_of(info, struct t1s_ptp, info);
    int ret;

    mutex_lock(&p->cmd_lock);
    ret = t1s_gettime(p, ts, sts);
    mutex_unlock(&p->cmd_lock);
    return ret;
}

static int t1s_ptp_settime64(struct ptp_clock_info* info, const struct timespec64* ts) {
    struct t1s_ptp* p = container_of(info, struct t1s_ptp, info);
    int ret;

    mutex_lock(&p->cmd_lock);
    ret = t1s_settime(p, ts);
    t1s_po_after_step(p);
    mutex_unlock(&p->cmd_lock);
    return ret;
}

static int t1s_ptp_adjtime(struct ptp_clock_info* info, s64 delta) {
    struct t1s_ptp* p = container_of(info, struct t1s_ptp, info);
    struct oa_tc6* tc6 = p->priv->tc6;
    struct timespec64 ts;
    int ret;

    mutex_lock(&p->cmd_lock);
    if (delta > -NSEC_PER_SEC && delta < NSEC_PER_SEC) {
        /* ADJ: added with the clock step, no cycle lost (t1s_mac_tsu) */
        ret = oa_tc6_write_register(tc6, T1S_PTP_ADJ, (u32)(s32)delta);
        if (!ret) {
            ret = t1s_cmd(tc6, T1S_CMD_ADJ);
        }
    } else {
        ret = t1s_gettime(p, &ts, NULL);
        if (!ret) {
            ts = timespec64_add(ts, ns_to_timespec64(delta));
            ret = t1s_settime(p, &ts);
        }
    }
    t1s_po_after_step(p);
    mutex_unlock(&p->cmd_lock);
    return ret;
}

static int t1s_ptp_adjfine(struct ptp_clock_info* info, long scaled_ppm) {
    struct t1s_ptp* p = container_of(info, struct t1s_ptp, info);
    u32 inc = (u32)adjust_by_scaled_ppm(T1S_INC_NOMINAL, scaled_ppm);
    int ret;

    mutex_lock(&p->cmd_lock);
    ret = t1s_set_inc(p->priv->tc6, inc);
    if (!ret) {
        p->inc = inc;
    }
    mutex_unlock(&p->cmd_lock);
    return ret;
}

/* ptp_clock_time -> ns, only for 0 <= value < 2^32 s; else U64_MAX */
static u64 t1s_pct_ns(const struct ptp_clock_time* t) {
    if (t->sec < 0 || t->sec > U32_MAX || t->nsec >= NSEC_PER_SEC) {
        return U64_MAX;
    }
    return (u64)t->sec * NSEC_PER_SEC + t->nsec;
}

/* Only PTP_CLK_REQ_PEROUT, index 0 = the MAC's pps pin.  No EXTTS (the MAC has
 * no time-stamped input) and no PTP_CLK_REQ_PPS (that is a kernel PPS event at
 * every PHC second, which needs a host interrupt with the PHC time; the MAC's
 * TS_STS.PPS is not routed to the host as an interrupt).
 *   period    1 s or less (PPS_PERIOD is at most 1e9 ns), high and low parts
 *             at least 20 ns (one clk_s cycle)
 *   width     perout.on with PTP_PEROUT_DUTY_CYCLE, else min(period / 2,
 *             100 ms) - 100 ms for 1 PPS (the MAC's reset value)
 *   start     edges at start + k * period on the PHC time scale; a start in
 *             the past (or within 10 ms) moves to the next such edge, so the
 *             phase is kept.  PTP_PEROUT_PHASE: start = phase (edges at
 *             phase + k * period, the first one as soon as possible)
 *   ONE_SHOT  not supported (the MAC output is periodic only)
 * A clock step (settime, adjtime) re-arms it on the same phase.
 */
static int t1s_ptp_enable(struct ptp_clock_info* info, struct ptp_clock_request* rq, int on) {
    struct t1s_ptp* p = container_of(info, struct t1s_ptp, info);
    const struct ptp_perout_request* po = &rq->perout;
    u64 period, width, start;
    int ret;

    if (rq->type != PTP_CLK_REQ_PEROUT) {
        return -EOPNOTSUPP;
    }
    if (po->index != 0) {
        return -EINVAL;
    }
    if (!on) {
        mutex_lock(&p->cmd_lock);
        p->po_on = false;
        ret = t1s_cmd(p->priv->tc6, T1S_CMD_PPS_STOP);
        mutex_unlock(&p->cmd_lock);
        return ret;
    }
    if (po->flags & ~(PTP_PEROUT_DUTY_CYCLE | PTP_PEROUT_PHASE)) {
        return -EOPNOTSUPP;
    }
    period = t1s_pct_ns(&po->period);
    if (period > NSEC_PER_SEC || period < 2 * T1S_PO_MIN_NS) {
        return -EINVAL; /* PPS_PERIOD <= 1e9 ns */
    }
    if (po->flags & PTP_PEROUT_DUTY_CYCLE) {
        width = t1s_pct_ns(&po->on);
    } else {
        width = min_t(u64, period / 2, T1S_PO_WIDTH_DEF_MAX);
    }
    if (width >= period || width < T1S_PO_MIN_NS || period - width < T1S_PO_MIN_NS) {
        return -EINVAL; /* 0 < PPS_WIDTH < PPS_PERIOD */
    }
    start = t1s_pct_ns(&po->start); /* same place as perout.phase */
    if (start == U64_MAX || ((po->flags & PTP_PEROUT_PHASE) && start >= period)) {
        return -EINVAL;
    }

    mutex_lock(&p->cmd_lock);
    p->po_start = start;
    p->po_period = (u32)period;
    p->po_width = (u32)width;
    ret = t1s_po_arm(p);
    if (ret) {
        p->po_on = false;
        p->po_errs++;
        t1s_cmd(p->priv->tc6, T1S_CMD_PPS_STOP); /* no earlier request left running */
    } else {
        p->po_on = true;
        p->po_arms++;
    }
    mutex_unlock(&p->cmd_lock);
    return ret;
}

/* ---- transmit ---- */

/* ndo_start_xmit (BH): pick a capture register for a frame that asks for a
 * hardware TX timestamp, keep a socket clone of it.  0 = no capture.
 */
u8 t1s_ptp_tx_request(struct lan865x_priv* priv, struct sk_buff* skb) {
    struct t1s_ptp* p = priv->t1s_ptp;
    struct sk_buff* clone;
    u8 slot = 0;

    if (!p || !p->tx_cap || !(skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP) ||
        READ_ONCE(priv->tstamp_config.tx_type) != HWTSTAMP_TX_ON) {
        return 0;
    }
    clone = skb_clone_sk(skb);
    if (!clone) {
        return 0;
    }
    spin_lock_bh(&p->slot_lock);
    for (int i = 0; i < T1S_NSLOT; i++) {
        u8 s = (p->next_slot + i - 1) % T1S_NSLOT + 1;

        if (!p->slot_skb[s]) {
            slot = s;
            break;
        }
    }
    if (slot) {
        p->slot_skb[slot] = clone;
        p->slot_start[slot] = jiffies;
        p->next_slot = slot % T1S_NSLOT + 1;
    } else {
        p->tx_noslot++;
    }
    spin_unlock_bh(&p->slot_lock);
    if (!slot) {
        dev_kfree_skb_any(clone);
        return 0;
    }
    skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
    wake_up(&p->wq);
    return slot;
}

static struct sk_buff* t1s_take_slot(struct t1s_ptp* p, u8 slot) {
    struct sk_buff* skb;

    spin_lock_bh(&p->slot_lock);
    skb = p->slot_skb[slot];
    p->slot_skb[slot] = NULL;
    spin_unlock_bh(&p->slot_lock);
    return skb;
}

/* the frame was not queued (NETDEV_TX_BUSY): the stack sends it again */
void t1s_ptp_tx_cancel(struct lan865x_priv* priv, u8 slot) {
    struct t1s_ptp* p = priv->t1s_ptp;
    struct sk_buff* skb;

    if (!p || !slot) {
        return;
    }
    skb = t1s_take_slot(p, slot);
    if (skb) {
        dev_kfree_skb_any(skb);
    }
}

static bool t1s_tx_pending(struct t1s_ptp* p) {
    for (int s = 1; s <= T1S_NSLOT; s++) {
        if (READ_ONCE(p->slot_skb[s])) {
            return true;
        }
    }
    return false;
}

static int t1s_ptp_thread(void* data) {
    struct t1s_ptp* p = data;
    struct oa_tc6* tc6 = p->priv->tc6;

    while (!kthread_should_stop()) {
        u32 sts, clr = 0;

        wait_event_interruptible(p->wq, t1s_tx_pending(p) || kthread_should_stop());
        if (kthread_should_stop()) {
            break;
        }
        usleep_range(t1s_ts_poll_us, t1s_ts_poll_us + 50);
        if (oa_tc6_read_register(tc6, T1S_TS_STS, &sts)) {
            continue;
        }
        for (u8 s = 1; s <= T1S_NSLOT; s++) {
            struct skb_shared_hwtstamps hw = {0};
            struct sk_buff* skb;
            u32 hi, lo;

            if (!READ_ONCE(p->slot_skb[s])) {
                continue;
            }
            if (sts & T1S_TS_STS_AVAIL(s)) {
                if (oa_tc6_read_register(tc6, T1S_TTSC_H(s), &hi) || oa_tc6_read_register(tc6, T1S_TTSC_L(s), &lo)) {
                    continue;
                }
                clr |= T1S_TS_STS_SLOT(s);
                skb = t1s_take_slot(p, s);
                if (skb) {
                    hw.hwtstamp = t1s_ts64(hi, lo);
                    skb_complete_tx_timestamp(skb, &hw);
                    p->tx_ok++;
                }
            } else if (sts & T1S_TS_STS_LOST(s)) {
                /* the frame did not go out (notes/54 D3) */
                clr |= T1S_TS_STS_SLOT(s);
                skb = t1s_take_slot(p, s);
                if (skb) {
                    dev_kfree_skb_any(skb);
                    p->tx_lost++;
                }
            } else if (time_after(jiffies, READ_ONCE(p->slot_start[s]) + msecs_to_jiffies(t1s_ts_timeout_ms))) {
                clr |= T1S_TS_STS_SLOT(s); /* a late capture must not answer the next request */
                skb = t1s_take_slot(p, s);
                if (skb) {
                    dev_kfree_skb_any(skb);
                    p->tx_timeout++;
                    net_warn_ratelimited("%s: t1s-oaspi: no TX timestamp in TTSC%c after %u ms (TS_STS 0x%08x, timeouts %llu)\n",
                                         netdev_name(p->priv->netdev), 'A' + s - 1, t1s_ts_timeout_ms, sts,
                                         p->tx_timeout);
                }
            }
        }
        if (clr) {
            oa_tc6_write_register(tc6, T1S_TS_STS, clr);
        }
    }
    return 0;
}

/* ---- MAC-PHY (re)initialization: t1s_oaspi_chip_init(), before CONFIG0.SYNC ---- */

int t1s_ptp_chip_init(struct oa_tc6* tc6, struct lan865x_priv* priv) {
    struct t1s_ptp* p = priv->t1s_ptp;
    u32 cap = 0;
    int ret;

    if (!t1s_ts) {
        oa_tc6_set_frame_ts(tc6, 0, NULL, NULL, false);
        return oa_tc6_write_register(tc6, T1S_MMS1_TS_CFG, 0);
    }
    ret = oa_tc6_write_register(tc6, T1S_TS_TX_LAT, (u32)t1s_ts_tx_lat_ns & 0xFFFFFF);
    if (!ret) {
        ret = oa_tc6_write_register(tc6, T1S_TS_RX_LAT, (u32)t1s_ts_rx_lat_ns & 0xFFFFFF);
    }
    /* after a reset (recovery) the clock runs at the nominal rate again */
    if (!ret && p && p->inc != T1S_INC_NOMINAL) {
        ret = t1s_set_inc(tc6, p->inc);
    }
    if (!ret) {
        ret = oa_tc6_write_register(tc6, T1S_MMS1_TS_CFG, T1S_TS_CFG_FTSE | T1S_TS_CFG_FTSS);
    }
    if (!ret) {
        ret = oa_tc6_read_register(tc6, T1S_TC6_STDCAP, &cap);
    }
    if (ret) {
        return ret;
    }
    /* FTSC = 1: the footer (RTSA / RTSP) says which frames carry a timestamp;
     * FTSC = 0 (older bitstream): every frame does, the length is 8 from TS_CFG.
     * Read again at every (re)init: a recovery follows a reset of the MAC-PHY.
     */
    oa_tc6_set_frame_ts(tc6, 8, t1s_ptp_rx_ts, priv, !!(cap & T1S_TC6_STDCAP_FTSC));
    return 0;
}

/* ---- ethtool / ioctl ---- */

int t1s_ptp_get_ts_info(struct lan865x_priv* priv, struct kernel_ethtool_ts_info* info) {
    struct t1s_ptp* p = priv->t1s_ptp;

    info->so_timestamping = SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
    info->phc_index = -1;
    if (!p) {
        return 0;
    }
    info->phc_index = ptp_clock_index(p->clock);
    info->so_timestamping |= SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE;
    info->tx_types = BIT(HWTSTAMP_TX_OFF);
    if (p->tx_cap) {
        info->so_timestamping |= SOF_TIMESTAMPING_TX_HARDWARE;
        info->tx_types |= BIT(HWTSTAMP_TX_ON);
    }
    info->rx_filters = BIT(HWTSTAMP_FILTER_NONE) | BIT(HWTSTAMP_FILTER_ALL);
    return 0;
}

int t1s_ptp_hwtstamp_get(struct lan865x_priv* priv, struct ifreq* ifr) {
    if (!priv->t1s_ptp) {
        return -EOPNOTSUPP;
    }
    return copy_to_user(ifr->ifr_data, &priv->tstamp_config, sizeof(priv->tstamp_config)) ? -EFAULT : 0;
}

int t1s_ptp_hwtstamp_set(struct lan865x_priv* priv, struct ifreq* ifr) {
    struct t1s_ptp* p = priv->t1s_ptp;
    struct hwtstamp_config cfg;

    if (!p) {
        return -EOPNOTSUPP;
    }
    if (copy_from_user(&cfg, ifr->ifr_data, sizeof(cfg))) {
        return -EFAULT;
    }
    if (cfg.flags) {
        return -EINVAL;
    }
    switch (cfg.tx_type) {
    case HWTSTAMP_TX_OFF:
        break;
    case HWTSTAMP_TX_ON:
        if (!p->tx_cap) {
            return -ERANGE; /* STDCAP.FTSC = 0: the MAC-PHY ignores TSC */
        }
        break;
    default:
        return -ERANGE;
    }
    /* every frame carries its timestamp: any filter is served as ALL */
    if (cfg.rx_filter != HWTSTAMP_FILTER_NONE) {
        cfg.rx_filter = HWTSTAMP_FILTER_ALL;
    }
    WRITE_ONCE(priv->tstamp_config.flags, cfg.flags);
    WRITE_ONCE(priv->tstamp_config.tx_type, cfg.tx_type);
    WRITE_ONCE(priv->tstamp_config.rx_filter, cfg.rx_filter);
    return copy_to_user(ifr->ifr_data, &cfg, sizeof(cfg)) ? -EFAULT : 0;
}

ssize_t t1s_ptp_stats(struct lan865x_priv* priv, char* buf, ssize_t n) {
    struct t1s_ptp* p = priv->t1s_ptp;
    u32 next_ns;
    u64 next_s;

    if (!p) {
        return n + sysfs_emit_at(buf, n, "ptp: off\n");
    }
    n += sysfs_emit_at(buf, n, "ptp: phc %d tx_cap %d inc 0x%08x tx_ok %llu tx_lost %llu tx_timeout %llu tx_noslot %llu\n",
                       ptp_clock_index(p->clock), p->tx_cap, p->inc, p->tx_ok, p->tx_lost, p->tx_timeout, p->tx_noslot);
    next_s = div_u64_rem(p->po_next, NSEC_PER_SEC, &next_ns);
    return n + sysfs_emit_at(buf, n, "perout: on %d period %u width %u first %llu.%09u arms %llu rearms %llu errs %llu\n",
                             p->po_on, p->po_period, p->po_width, next_s, next_ns, p->po_arms, p->po_rearms, p->po_errs);
}

/* ---- probe / remove ---- */

int t1s_ptp_init(struct lan865x_priv* priv) {
    struct t1s_ptp* p;
    u32 cap = 0;

    if (!t1s_ts) {
        return 0;
    }
    p = kzalloc(sizeof(*p), GFP_KERNEL);
    if (!p) {
        return -ENOMEM;
    }
    p->priv = priv;
    p->inc = T1S_INC_NOMINAL;
    p->next_slot = 1;
    mutex_init(&p->cmd_lock);
    spin_lock_init(&p->slot_lock);
    init_waitqueue_head(&p->wq);

    if (oa_tc6_read_register(priv->tc6, T1S_TC6_STDCAP, &cap)) {
        cap = 0;
    }
    p->tx_cap = (cap & T1S_TC6_STDCAP_FTSC) || t1s_ts_tx_force;

    p->info.owner = THIS_MODULE;
    snprintf(p->info.name, sizeof(p->info.name), "t1s-oaspi");
    p->info.max_adj = 1000000; /* ppb; PTP_INC Q8.24 resolves 3e-9 */
    p->info.adjfine = t1s_ptp_adjfine;
    p->info.adjtime = t1s_ptp_adjtime;
    p->info.gettimex64 = t1s_ptp_gettimex64;
    p->info.settime64 = t1s_ptp_settime64;
    p->info.enable = t1s_ptp_enable;
    p->info.n_per_out = 1; /* the MAC's pps pin (fixed, no pin configuration) */

    p->clock = ptp_clock_register(&p->info, &priv->spi->dev);
    if (IS_ERR_OR_NULL(p->clock)) {
        int ret = p->clock ? PTR_ERR(p->clock) : -ENODEV;

        kfree(p);
        return ret;
    }
    p->thread = kthread_run(t1s_ptp_thread, p, "t1s-ptp");
    if (IS_ERR(p->thread)) {
        int ret = PTR_ERR(p->thread);

        ptp_clock_unregister(p->clock);
        kfree(p);
        return ret;
    }
    priv->t1s_ptp = p;
    dev_info(&priv->spi->dev, "t1s-oaspi: PTP clock ptp%d, RX timestamps on, TX timestamps %s (STDCAP 0x%08x%s)\n",
             ptp_clock_index(p->clock), p->tx_cap ? "offered" : "not offered", cap,
             t1s_ts_tx_force ? ", forced" : "");
    return 0;
}

void t1s_ptp_exit(struct lan865x_priv* priv) {
    struct t1s_ptp* p = priv->t1s_ptp;

    if (!p) {
        return;
    }
    kthread_stop(p->thread);
    for (u8 s = 1; s <= T1S_NSLOT; s++) {
        struct sk_buff* skb = t1s_take_slot(p, s);

        if (skb) {
            dev_kfree_skb_any(skb);
        }
    }
    ptp_clock_unregister(p->clock);
    /* no request can come any more: leave the pin low (best effort) */
    mutex_lock(&p->cmd_lock);
    if (p->po_on) {
        p->po_on = false;
        t1s_cmd(priv->tc6, T1S_CMD_PPS_STOP);
    }
    mutex_unlock(&p->cmd_lock);
    priv->t1s_ptp = NULL;
    kfree(p);
}
#endif /* FRAME_TIMESTAMP_ENABLE */
