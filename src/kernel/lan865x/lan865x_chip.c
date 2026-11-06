// SPDX-License-Identifier: GPL-2.0+
/*
 * Microchip LAN8650/1 MAC-PHY specific part of the OPEN Alliance TC6 driver:
 * the init step (AN1760 configuration, PLCA / MAC enable, CONFIG0.SYNC) and
 * the vendor-specific STATUS1 / IMASK1 layout (OPEN Alliance TC6 v1.1 9.2.9 /
 * 9.2.12 leave both registers to the vendor; DS60001734F 11.1.7 / 11.1.10).
 *
 * Moved from oa_tc6.c unchanged (DRV-1): oa_tc6.c keeps the TC6 standard
 * part only and gets this through struct oa_tc6_chip_ops.
 */

#include <linux/bitops.h>
#include <linux/kernel.h>

#include "../../../include/linux/oa_tc6.h"
#include "lan865x_chip.h"

/* PHY Vendor Specific Registers Collision Detector Control 0 Register */
#define LAN8650_REG_MMS4_CDCTL0 0x00040087
#define MMS4_CDCTL0_CDEN_SHIFT BIT(15) /* Collision Detect Enable */

#define LAN8650_REG_MMS4_PLCA_CTRL0 0x0004CA01
#define LAN8650_REG_MMS1_MAC_NCFGR 0x00010001
#define LAN8650_REG_MMS1_MAC_NCR 0x00010000
#define LAN8650_REG_MMS1_MAC_TI 0x00010077
#define LAN8650_REG_MMS0_OA_CONFIG0 0x00000004
#define MMS0_OA_CONFIG0_SYNC_SHIFT BIT(15)
#define MMS0_OA_CONFIG0_FTSE_SHIFT BIT(7)
#define MMS0_OA_CONFIG0_FTSS_SHIFT BIT(6)
#define LAN8650_REG_MMS0_OA_CONFIG0 0x00000004
#define LAN8650_REG_MMS0_OA_STATUS0 0x00000008
#define MMS0_OA_STATUS0_RESETC_SHIFT BIT(6)

#define LAN8650_REG_MMS4_INDIR_RD_ADDR 0x000400d8
#define LAN8650_REG_MMS4_INDIR_RD_VAL 0x000400d9
#define LAN8650_REG_MMS4_INDIR_RD_WIDTH 0x000400da
#define LAN8650_MMS04_INDIR_WIDTH 0x2

#define LAN8650_REG_MMS4_SLPCTL1 0x00040081
#define LAN8650_REG_MMS4_TXMMSKH 0x00040043
#define LAN8650_REG_MMS4_TXMMSKL 0x00040044
#define LAN8650_REG_MMS4_TXMLOC 0x00040045
#define LAN8650_REG_MMS4_RXMMSKH 0x00040053
#define LAN8650_REG_MMS4_RXMMSKL 0x00040054
#define LAN8650_REG_MMS4_RXMLOC 0x00040055
#define LAN8650_REG_MMS4_TXMCTL 0x00040040
#define LAN8650_REG_MMS4_RXMCTL 0x00040050

#define LAN8650_REG_MMS4_A_00D0 0x000400d0
#define LAN8650_REG_MMS4_A_00E0 0x000400e0
#define LAN8650_REG_MMS4_A_0077 0x00040077
#define LAN8650_REG_MMS4_A_0084 0x00040084
#define LAN8650_REG_MMS4_A_008A 0x0004008a
#define LAN8650_REG_MMS4_A_0091 0x00040091
#define LAN8650_REG_MMS4_A_00E9 0x000400e9
#define LAN8650_REG_MMS4_A_00F5 0x000400f5
#define LAN8650_REG_MMS4_A_00F4 0x000400f4
#define LAN8650_REG_MMS4_A_00F8 0x000400f8
#define LAN8650_REG_MMS4_A_00F9 0x000400f9

/* Value1: -5 to 15 */
#define VALUE1_ADDR 0x04
#define VALUE2_ADDR 0x08
#define VALUE_MASK 0x1F
#define VALUE_SIGN_MASK 0x10

#define VALUE1_OFFSET1 9
#define VALUE1_OFFSET2 14
#define VALUE1_SHIFT1 10
#define VALUE1_SHIFT2 4
#define VALUE1_LOWEST_VAL 0x03

#define VALUE2_OFFSET 40
#define VALUE2_SHIFT 10

#define VALUE_OFFSET_MASK 0x3F

#define MAX_VALUE_5BITS 0x20

#define MMS4_A_00D0_V 0x3F31
#define MMS4_A_00E0_V 0xC000
#define MMS4_A_00E9_V 0x9E50
#define MMS4_A_00F5_V 0x1CF8
#define MMS4_A_00F4_V 0xC020
#define MMS4_A_00F8_V 0xB900
#define MMS4_A_00F9_V 0x4E53
#define MMS4_A_0081_V 0x0080
#define MMS4_A_0091_V 0x9660
#define MMS4_A_0077_V 0x0028
#define MMS4_A_0043_V 0x00FF
#define MMS4_A_0044_V 0xFFFF
#define MMS4_A_0045_V 0x0000
#define MMS4_A_0053_V 0x00FF
#define MMS4_A_0054_V 0xFFFF
#define MMS4_A_0055_V 0x0000
#define MMS4_A_0040_V 0x0002
#define MMS4_A_0050_V 0x0002

#define MMS4_PLCA_CTRL0_INIT_VAL 0x00008000
#define MMS1_MAC_NCFGR_INIT_VAL 0x000000C0
#define MMS1_MAC_NCR_INIT_VAL 0x0000000C

#define TIMER_INCREMENT 0x28

u8 indirect_read(struct oa_tc6 *tc6, u8 addr, u8 mask)
{
	u32 regval;

	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_INDIR_RD_ADDR, addr);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_INDIR_RD_WIDTH,
			      LAN8650_MMS04_INDIR_WIDTH);

	oa_tc6_read_register(tc6, LAN8650_REG_MMS4_INDIR_RD_VAL, &regval);

	return (regval & mask);
}

static void set_macphy_register(struct oa_tc6 *tc6)
{
	u8 value1, value2;
	char offset1, offset2;
	u16 cfgparam1, cfgparam2;

	value1 = indirect_read(tc6, VALUE1_ADDR, VALUE_MASK);
	if ((value1 & VALUE_SIGN_MASK) != 0) {
		offset1 = (int8_t)((u8)value1 - MAX_VALUE_5BITS);
	} else {
		offset1 = (int8_t)value1;
	}

	value2 = indirect_read(tc6, VALUE2_ADDR, VALUE_MASK);
	if ((value2 & VALUE_SIGN_MASK) != 0) {
		offset2 = (int8_t)((u8)value2 - MAX_VALUE_5BITS);
	} else {
		offset2 = (int8_t)value2;
	}

	cfgparam1 = (u16)(((VALUE1_OFFSET1 + offset1) & VALUE_OFFSET_MASK)
			  << VALUE1_SHIFT1) |
		    (u16)(((VALUE1_OFFSET2 + offset1) & VALUE_OFFSET_MASK)
			  << VALUE1_SHIFT2) |
		    VALUE1_LOWEST_VAL;
	cfgparam2 = (u16)(((VALUE2_OFFSET + offset2) & VALUE_OFFSET_MASK)
			  << VALUE2_SHIFT);

	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_A_00D0, MMS4_A_00D0_V);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_A_00E0, MMS4_A_00E0_V);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_A_0084, cfgparam1);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_A_008A, cfgparam2);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_A_00E9, MMS4_A_00E9_V);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_A_00F5, MMS4_A_00F5_V);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_A_00F4, MMS4_A_00F4_V);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_A_00F8, MMS4_A_00F8_V);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_A_00F9, MMS4_A_00F9_V);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_SLPCTL1, MMS4_A_0081_V);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_A_0091, MMS4_A_0091_V);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_A_0077, MMS4_A_0077_V);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_TXMMSKH, MMS4_A_0043_V);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_TXMMSKL, MMS4_A_0044_V);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_TXMLOC, MMS4_A_0045_V);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_RXMMSKH, MMS4_A_0053_V);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_RXMMSKL, MMS4_A_0054_V);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_RXMLOC, MMS4_A_0055_V);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_TXMCTL, MMS4_A_0040_V);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_RXMCTL, MMS4_A_0050_V);

	/* (Optional) Write SQI Configuration into Registers */
#if 0
	u16 cfgparam3, cfgparam4, cfgparam5;

	cfgparam3 = (u16)(((5 + offset1) & 0x3F) << 8) |
		    (u16)((9 + offset1) & 0x3F);
	cfgparam4 = (u16)(((9 + offset1) & 0x3F) << 8) |
		    (u16)((14 + offset1) & 0x3F);
	cfgparam5 = (u16)(((17 + offset1) & 0x3F) << 8) |
		    (u16)((22 + offset1) & 0x3F);

	oa_tc6_write_register(tc6, 0x000400AD, cfgparam3);
	oa_tc6_write_register(tc6, 0x000400AE, cfgparam4);
	oa_tc6_write_register(tc6, 0x000400AF, cfgparam5);
	oa_tc6_write_register(tc6, 0x000400B0, 0x0103);
	oa_tc6_write_register(tc6, 0x000400B1, 0x0910);
	oa_tc6_write_register(tc6, 0x000400B2, 0x1D26);
	oa_tc6_write_register(tc6, 0x000400B3, 0x002A);
	oa_tc6_write_register(tc6, 0x000400B4, 0x0103);
	oa_tc6_write_register(tc6, 0x000400B5, 0x070D);
	oa_tc6_write_register(tc6, 0x000400B6, 0x1720);
	oa_tc6_write_register(tc6, 0x000400B7, 0x0027);
	oa_tc6_write_register(tc6, 0x000400B8, 0x0509);
	oa_tc6_write_register(tc6, 0x000400B9, 0x0E13);
	oa_tc6_write_register(tc6, 0x000400BA, 0x1C25);
	oa_tc6_write_register(tc6, 0x000400BB, 0x002B);
#endif
}

int init_lan865x(struct oa_tc6 *tc6)
{
	u32 regval;
	int ret;

	/* Read OA_STATUS0 */
	ret = oa_tc6_read_register(tc6, LAN8650_REG_MMS0_OA_STATUS0, &regval);
	if (ret)
		return ret;

	/* Write 1 to RESETC bit of OA_STATUS0 */
	regval |= MMS0_OA_STATUS0_RESETC_SHIFT;
	ret = oa_tc6_write_register(tc6, LAN8650_REG_MMS0_OA_STATUS0, regval);
	if (ret)
		return ret;

	/* Read Collision Detector Control 0 Register */
	ret = oa_tc6_read_register(tc6, LAN8650_REG_MMS4_CDCTL0, &regval);
	if (ret)
		return ret;

	/* Collision detection is enabled (default) */
	regval |= MMS4_CDCTL0_CDEN_SHIFT;
	ret = oa_tc6_write_register(tc6, LAN8650_REG_MMS4_CDCTL0, regval);
	if (ret)
		return ret;

	set_macphy_register(tc6);

	oa_tc6_write_register(tc6, LAN8650_REG_MMS4_PLCA_CTRL0,
			      MMS4_PLCA_CTRL0_INIT_VAL); /* Enable PLCA */
	oa_tc6_write_register(
		tc6, LAN8650_REG_MMS1_MAC_NCFGR,
		MMS1_MAC_NCFGR_INIT_VAL); /* Enable unicast, multicast */
	oa_tc6_write_register(tc6, LAN8650_REG_MMS1_MAC_NCR,
			      MMS1_MAC_NCR_INIT_VAL); /* Enable MACPHY TX, RX */

#ifdef FRAME_TIMESTAMP_ENABLE
	oa_tc6_write_register(tc6, LAN8650_REG_MMS1_MAC_TI,
			      TIMER_INCREMENT); /* Enable MACPHY TX, RX */
#endif

	/* Read OA_CONFIG0 */
	oa_tc6_read_register(tc6, LAN8650_REG_MMS0_OA_CONFIG0, &regval);

	/* Set SYNC bit of OA_CONFIG0 */
	regval |= MMS0_OA_CONFIG0_SYNC_SHIFT;
#ifdef FRAME_TIMESTAMP_ENABLE
	/* Set FTSE Frame Timestamp Enable bit of OA_CONFIG0 */
	regval |= MMS0_OA_CONFIG0_FTSE_SHIFT;

	/* Set FTSS Frame Timestamp Select bit of OA_CONFIG0 */
	regval |= MMS0_OA_CONFIG0_FTSS_SHIFT;
#endif
	oa_tc6_write_register(tc6, LAN8650_REG_MMS0_OA_CONFIG0, regval);

	/* Read OA_STATUS0 */
	oa_tc6_read_register(tc6, LAN8650_REG_MMS0_OA_STATUS0, &regval);

	/* Clear RESETC bit of OA_STATUS0 */
	regval &= ~(MMS0_OA_STATUS0_RESETC_SHIFT);
	oa_tc6_write_register(tc6, LAN8650_REG_MMS0_OA_STATUS0, regval);

	return 0;
}

/* OA_STATUS1 (0x0009) / OA_IMASK1 (0x000D) bits of the LAN8650/1.
 * DS60001734F 4.6.2: on TXNER/RXNER the transfer of Ethernet frames through
 * the device is halted and "the host controller must perform a hardware or
 * software reset"; the same applies to FSMSTER and BUSER.  All of them are
 * masked at reset (OA_IMASK1 reset value).
 */
#define LAN865X_STATUS1_RXNER BIT(0)
#define LAN865X_STATUS1_TXNER BIT(1)
#define LAN865X_STATUS1_FSMSTER BIT(17)
#define LAN865X_STATUS1_ECC BIT(18)
#define LAN865X_STATUS1_UV18 BIT(19)
#define LAN865X_STATUS1_BUSER BIT(20)

const struct oa_tc6_chip_ops lan865x_chip_ops = {
	.chip_init = init_lan865x,
	/* IMASK1 RXNERM / TXNERM / FSMSTERM / ECCM / BUSERM */
	.imask1_unmask = LAN865X_STATUS1_RXNER | LAN865X_STATUS1_TXNER |
			 LAN865X_STATUS1_FSMSTER | LAN865X_STATUS1_ECC |
			 LAN865X_STATUS1_BUSER,
	.status1_handled = LAN865X_STATUS1_RXNER | LAN865X_STATUS1_TXNER |
			   LAN865X_STATUS1_FSMSTER | LAN865X_STATUS1_ECC |
			   LAN865X_STATUS1_UV18 | LAN865X_STATUS1_BUSER,
	.status1_fatal = LAN865X_STATUS1_TXNER | LAN865X_STATUS1_RXNER |
			 LAN865X_STATUS1_FSMSTER | LAN865X_STATUS1_BUSER,
	.status1_warn = LAN865X_STATUS1_ECC | LAN865X_STATUS1_UV18,
};
