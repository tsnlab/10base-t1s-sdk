// SPDX-License-Identifier: GPL-2.0+
/*
 * OPEN Alliance 10BASE‑T1x MAC‑PHY Serial Interface framework
 *
 * Author: Parthiban Veerasooran <parthiban.veerasooran@microchip.com>
 */

#include "../../include/linux/oa_tc6.h" /* SDK copy (see lan865x_arch.h) */
#ifdef FRAME_TIMESTAMP_ENABLE
#include "lan865x/lan865x_arch.h"
#include <linux/ptp_classify.h>
#include <linux/if_vlan.h>
#include <linux/time64.h>
#endif /* FRAME_TIMESTAMP_ENABLE */
#include <linux/bitfield.h>
#include <linux/iopoll.h>
#include <linux/phy.h>
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spi/spi.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/phy.h>
#include <linux/mdio.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/iopoll.h>


/* OPEN Alliance TC6 registers */
/* Standard Capabilities Register */
#define OA_TC6_REG_STDCAP 0x0002
#define STDCAP_DIRECT_PHY_REG_ACCESS BIT(8)

/* Reset Control and Status Register */
#define OA_TC6_REG_RESET 0x0003
#define RESET_SWRESET BIT(0) /* Software Reset */

/* Configuration Register #0 */
#define OA_TC6_REG_CONFIG0 0x0004
#define CONFIG0_SYNC BIT(15)
#define CONFIG0_ZARFE_ENABLE BIT(12)

/* Status Register #0 */
#define OA_TC6_REG_STATUS0 0x0008
#define STATUS0_RESETC BIT(6) /* Reset Complete */
#define STATUS0_HEADER_ERROR BIT(5)
#define STATUS0_LOSS_OF_FRAME_ERROR BIT(4)
#define STATUS0_RX_BUFFER_OVERFLOW_ERROR BIT(3)
#define STATUS0_TX_BUFFER_UNDERFLOW_ERROR BIT(2)
#define STATUS0_TX_BUFFER_OVERFLOW_ERROR BIT(1)
#define STATUS0_TX_PROTOCOL_ERROR BIT(0)
#define STATUS0_TXFCSE BIT(11)
#define STATUS0_CPDE BIT(12)

/* Status Register #1 (LAN8650/1 vendor specific, DS60001734F 11.1.7) */
#define OA_TC6_REG_STATUS1 0x0009
#define STATUS1_RXNER BIT(0)
#define STATUS1_TXNER BIT(1)
#define STATUS1_FSMSTER BIT(17)
#define STATUS1_ECC BIT(18)
#define STATUS1_UV18 BIT(19)
#define STATUS1_BUSER BIT(20)

/* Buffer Status Register */
#define OA_TC6_REG_BUFFER_STATUS 0x000B
#define BUFFER_STATUS_TX_CREDITS_AVAILABLE GENMASK(15, 8)
#define BUFFER_STATUS_RX_CHUNKS_AVAILABLE GENMASK(7, 0)

/* Interrupt Mask Register #0 */
#define OA_TC6_REG_INT_MASK0 0x000C
#define INT_MASK0_HEADER_ERR_MASK BIT(5)
#define INT_MASK0_LOSS_OF_FRAME_ERR_MASK BIT(4)
#define INT_MASK0_RX_BUFFER_OVERFLOW_ERR_MASK BIT(3)
#define INT_MASK0_TX_BUFFER_UNDERFLOW_ERR_MASK BIT(2)
#define INT_MASK0_TX_BUFFER_OVERFLOW_ERR_MASK BIT(1)
#define INT_MASK0_TX_PROTOCOL_ERR_MASK BIT(0)
#define INT_MASK0_ALL_INTERRUPTS (GENMASK(5, 0) | GENMASK(12, 7))

/* Interrupt Mask Register #1 (DS60001734F 11.1.10) */
#define OA_TC6_REG_INT_MASK1 0x000D
#define INT_MASK1_RXNERM BIT(0)
#define INT_MASK1_TXNERM BIT(1)
#define INT_MASK1_FSMSTERM BIT(17)
#define INT_MASK1_ECCM BIT(18)
#define INT_MASK1_BUSERM BIT(20)

/* PHY Clause 22 registers base address and mask */
#define OA_TC6_PHY_STD_REG_ADDR_BASE 0xFF00
#define OA_TC6_PHY_STD_REG_ADDR_MASK 0x1F

/* Control command header */
#define OA_TC6_CTRL_HEADER_DATA_NOT_CTRL BIT(31)
#define OA_TC6_CTRL_HEADER_WRITE_NOT_READ BIT(29)
#define OA_TC6_CTRL_HEADER_MEM_MAP_SELECTOR GENMASK(27, 24)
#define OA_TC6_CTRL_HEADER_ADDR GENMASK(23, 8)
#define OA_TC6_CTRL_HEADER_LENGTH GENMASK(7, 1)
#define OA_TC6_CTRL_HEADER_PARITY BIT(0)

/* Data header */
#define OA_TC6_DATA_HEADER_DATA_NOT_CTRL BIT(31)
#define OA_TC6_DATA_HEADER_DATA_VALID BIT(21)
#define OA_TC6_DATA_HEADER_START_VALID BIT(20)
#define OA_TC6_DATA_HEADER_START_WORD_OFFSET GENMASK(19, 16)
#define OA_TC6_DATA_HEADER_END_VALID BIT(14)
#define OA_TC6_DATA_HEADER_END_BYTE_OFFSET GENMASK(13, 8)
#ifdef FRAME_TIMESTAMP_ENABLE
#define OA_TC6_DATA_HEADER_TIMESTAMP_CAPTURE GENMASK(7, 6)
#endif /* FRAME_TIMESTAMP_ENABLE */
#define OA_TC6_DATA_HEADER_PARITY BIT(0)

/* Data footer */
#define OA_TC6_DATA_FOOTER_EXTENDED_STS BIT(31)
#define OA_TC6_DATA_FOOTER_RXD_HEADER_BAD BIT(30)
#define OA_TC6_DATA_FOOTER_CONFIG_SYNC BIT(29)
#define OA_TC6_DATA_FOOTER_RX_CHUNKS GENMASK(28, 24)
#define OA_TC6_DATA_FOOTER_DATA_VALID BIT(21)
#define OA_TC6_DATA_FOOTER_START_VALID BIT(20)
#define OA_TC6_DATA_FOOTER_START_WORD_OFFSET GENMASK(19, 16)
#define OA_TC6_DATA_FOOTER_FRAME_DROP BIT(15)
#define OA_TC6_DATA_FOOTER_END_VALID BIT(14)
#define OA_TC6_DATA_FOOTER_END_BYTE_OFFSET GENMASK(13, 8)
#define OA_TC6_DATA_FOOTER_TX_CREDITS GENMASK(5, 1)

/* PHY – Clause 45 registers memory map selector (MMS) as per table 6 in the
 * OPEN Alliance specification.
 */
#define OA_TC6_PHY_C45_PCS_MMS2 2 /* MMD 3 */
#define OA_TC6_PHY_C45_PMA_PMD_MMS3 3 /* MMD 1 */
#define OA_TC6_PHY_C45_VS_PLCA_MMS4 4 /* MMD 31 */
#define OA_TC6_PHY_C45_AUTO_NEG_MMS5 5 /* MMD 7 */
#define OA_TC6_PHY_C45_POWER_UNIT_MMS6 6 /* MMD 13 */

#define OA_TC6_CTRL_HEADER_SIZE 4
#define OA_TC6_CTRL_REG_VALUE_SIZE 4
#define OA_TC6_CTRL_IGNORED_SIZE 4
#define OA_TC6_CTRL_MAX_REGISTERS 128
#define OA_TC6_CTRL_SPI_BUF_SIZE                                    \
	(OA_TC6_CTRL_HEADER_SIZE +                                  \
	 (OA_TC6_CTRL_MAX_REGISTERS * OA_TC6_CTRL_REG_VALUE_SIZE) + \
	 OA_TC6_CTRL_IGNORED_SIZE)
#define OA_TC6_CHUNK_PAYLOAD_SIZE 64
#define OA_TC6_DATA_HEADER_SIZE 4
#define OA_TC6_CHUNK_SIZE (OA_TC6_DATA_HEADER_SIZE + OA_TC6_CHUNK_PAYLOAD_SIZE)
#define OA_TC6_MAX_TX_CHUNKS 48
#define OA_TC6_SPI_DATA_BUF_SIZE (OA_TC6_MAX_TX_CHUNKS * OA_TC6_CHUNK_SIZE)
#define STATUS0_RESETC_POLL_DELAY 1000
#define STATUS0_RESETC_POLL_TIMEOUT 1000000

#ifdef FRAME_TIMESTAMP_ENABLE
#define OA_TC6_TIMESTAMP_SIZE 8
#define OA_TC6_UNNECESSARY_FOOTER_SIZE 4
#endif /* FRAME_TIMESTAMP_ENABLE */

/* Internal structure for MAC-PHY drivers */
struct oa_tc6 {
	struct device *dev;
	struct net_device *netdev;
	struct phy_device *phydev;
	struct mii_bus *mdiobus;
	struct spi_device *spi;
	struct mutex spi_ctrl_lock; /* Protects spi control transfer */
	spinlock_t tx_skb_lock; /* Protects tx skb handling */
	void *spi_ctrl_tx_buf;
	void *spi_ctrl_rx_buf;
	void *spi_data_tx_buf;
	void *spi_data_rx_buf;
	struct sk_buff *ongoing_tx_skb;
	struct sk_buff *waiting_tx_skb;
	struct sk_buff *rx_skb;
	u16 tx_skb_offset;
	u16 spi_data_tx_buf_offset;
	u16 tx_credits;
	u8 rx_chunks_available;
	bool wait_until_start_valid;
	bool int_flag;
	u8 footer_par_err_run; /* consecutive footers with a parity error */
	bool disable_traffic;
	bool suspended; /* data path parked by oa_tc6_stop_traffic() */
	unsigned long irq_flags; /* OA_TC6_IRQ_OFF */
	u32 irq_type_orig; /* trigger type of the IRQ mapping before request */
	void (*fatal_cb)(void *ctx);
	void *fatal_ctx;
	/* MAC-PHY specific part of the init / reinit sequence (after the
	 * TC6 software reset and error IRQ unmask, before the data
	 * transfer is enabled).  init_lan865x() unless the MAC driver gave
	 * another one to oa_tc6_init_chip().
	 */
	int (*chip_init)(struct oa_tc6 *tc6);
	u64 stats[OA_TC6_STAT_NUM];
#ifdef FRAME_TIMESTAMP_ENABLE
	u8 ongoing_tx_ts_capture_mode;
	u8 waiting_tx_ts_capture_mode;
#endif /* FRAME_TIMESTAMP_ENABLE */
};

enum oa_tc6_header_type {
	OA_TC6_CTRL_HEADER,
	OA_TC6_DATA_HEADER,
};

enum oa_tc6_register_op {
	OA_TC6_CTRL_REG_READ = 0,
	OA_TC6_CTRL_REG_WRITE = 1,
};

enum oa_tc6_data_valid_info {
	OA_TC6_DATA_INVALID,
	OA_TC6_DATA_VALID,
};

enum oa_tc6_data_start_valid_info {
	OA_TC6_DATA_START_INVALID,
	OA_TC6_DATA_START_VALID,
};

enum oa_tc6_data_end_valid_info {
	OA_TC6_DATA_END_INVALID,
	OA_TC6_DATA_END_VALID,
};

static int oa_tc6_spi_transfer(struct oa_tc6 *tc6,
			       enum oa_tc6_header_type header_type, u16 length)
{
	struct spi_transfer xfer = { 0 };
	struct spi_message msg;

	if (header_type == OA_TC6_DATA_HEADER) {
		xfer.tx_buf = tc6->spi_data_tx_buf;
		xfer.rx_buf = tc6->spi_data_rx_buf;
	} else {
		xfer.tx_buf = tc6->spi_ctrl_tx_buf;
		xfer.rx_buf = tc6->spi_ctrl_rx_buf;
	}
	xfer.len = length;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	return spi_sync(tc6->spi, &msg);
}

static int oa_tc6_get_parity(u32 p)
{
	/* Public domain code snippet, lifted from
	 * http://www-graphics.stanford.edu/~seander/bithacks.html
	 */
	p ^= p >> 1;
	p ^= p >> 2;
	p = (p & 0x11111111U) * 0x11111111U;

	/* Odd parity is used here */
	return !((p >> 28) & 1);
}

static __be32 oa_tc6_prepare_ctrl_header(u32 addr, u8 length,
					 enum oa_tc6_register_op reg_op)
{
	u32 header;

	header = FIELD_PREP(OA_TC6_CTRL_HEADER_DATA_NOT_CTRL,
			    OA_TC6_CTRL_HEADER) |
		 FIELD_PREP(OA_TC6_CTRL_HEADER_WRITE_NOT_READ, reg_op) |
		 FIELD_PREP(OA_TC6_CTRL_HEADER_MEM_MAP_SELECTOR, addr >> 16) |
		 FIELD_PREP(OA_TC6_CTRL_HEADER_ADDR, addr) |
		 FIELD_PREP(OA_TC6_CTRL_HEADER_LENGTH, length - 1);
	header |= FIELD_PREP(OA_TC6_CTRL_HEADER_PARITY,
			     oa_tc6_get_parity(header));

	return cpu_to_be32(header);
}

static void oa_tc6_update_ctrl_write_data(struct oa_tc6 *tc6, u32 value[],
					  u8 length)
{
	__be32 *tx_buf = tc6->spi_ctrl_tx_buf + OA_TC6_CTRL_HEADER_SIZE;

	for (int i = 0; i < length; i++)
		*tx_buf++ = cpu_to_be32(value[i]);
}

static u16 oa_tc6_calculate_ctrl_buf_size(u8 length)
{
	/* Control command consists 4 bytes header + 4 bytes register value for
	 * each register + 4 bytes ignored value.
	 */
	return OA_TC6_CTRL_HEADER_SIZE + OA_TC6_CTRL_REG_VALUE_SIZE * length +
	       OA_TC6_CTRL_IGNORED_SIZE;
}

static void oa_tc6_prepare_ctrl_spi_buf(struct oa_tc6 *tc6, u32 address,
					u32 value[], u8 length,
					enum oa_tc6_register_op reg_op)
{
	__be32 *tx_buf = tc6->spi_ctrl_tx_buf;

	*tx_buf = oa_tc6_prepare_ctrl_header(address, length, reg_op);

	if (reg_op == OA_TC6_CTRL_REG_WRITE)
		oa_tc6_update_ctrl_write_data(tc6, value, length);
}

static int oa_tc6_check_ctrl_write_reply(struct oa_tc6 *tc6, u8 size)
{
	u8 *tx_buf = tc6->spi_ctrl_tx_buf;
	u8 *rx_buf = tc6->spi_ctrl_rx_buf;

	rx_buf += OA_TC6_CTRL_IGNORED_SIZE;

	/* The echoed control write must match with the one that was
	 * transmitted.
	 */
	if (memcmp(tx_buf, rx_buf, size - OA_TC6_CTRL_IGNORED_SIZE))
		return -EPROTO;

	return 0;
}

static int oa_tc6_check_ctrl_read_reply(struct oa_tc6 *tc6, u8 size)
{
	u32 *rx_buf = tc6->spi_ctrl_rx_buf + OA_TC6_CTRL_IGNORED_SIZE;
	u32 *tx_buf = tc6->spi_ctrl_tx_buf;

	/* The echoed control read header must match with the one that was
	 * transmitted.
	 */
	if (*tx_buf != *rx_buf)
		return -EPROTO;

	return 0;
}

static void oa_tc6_copy_ctrl_read_data(struct oa_tc6 *tc6, u32 value[],
				       u8 length)
{
	__be32 *rx_buf = tc6->spi_ctrl_rx_buf + OA_TC6_CTRL_IGNORED_SIZE +
			 OA_TC6_CTRL_HEADER_SIZE;

	for (int i = 0; i < length; i++)
		value[i] = be32_to_cpu(*rx_buf++);
}

static int oa_tc6_perform_ctrl(struct oa_tc6 *tc6, u32 address, u32 value[],
			       u8 length, enum oa_tc6_register_op reg_op)
{
	u16 size;
	int ret;

	/* Prepare control command and copy to SPI control buffer */
	oa_tc6_prepare_ctrl_spi_buf(tc6, address, value, length, reg_op);

	size = oa_tc6_calculate_ctrl_buf_size(length);

	/* Perform SPI transfer */
	ret = oa_tc6_spi_transfer(tc6, OA_TC6_CTRL_HEADER, size);
	if (ret) {
		dev_err(&tc6->spi->dev, "SPI transfer failed for control: %d\n",
			ret);
		return ret;
	}

	/* Check echoed/received control write command reply for errors */
	if (reg_op == OA_TC6_CTRL_REG_WRITE)
		return oa_tc6_check_ctrl_write_reply(tc6, size);

	/* Check echoed/received control read command reply for errors */
	ret = oa_tc6_check_ctrl_read_reply(tc6, size);
	if (ret)
		return ret;

	oa_tc6_copy_ctrl_read_data(tc6, value, length);

	return 0;
}

/**
 * oa_tc6_read_registers - function for reading multiple consecutive registers.
 * @tc6: oa_tc6 struct.
 * @address: address of the first register to be read in the MAC-PHY.
 * @value: values to be read from the starting register address @address.
 * @length: number of consecutive registers to be read from @address.
 *
 * Maximum of 128 consecutive registers can be read starting at @address.
 *
 * Return: 0 on success otherwise failed.
 */
int oa_tc6_read_registers(struct oa_tc6 *tc6, u32 address, u32 value[],
			  u8 length)
{
	int ret;

	if (!length || length > OA_TC6_CTRL_MAX_REGISTERS) {
		dev_err(&tc6->spi->dev, "Invalid register length parameter\n");
		return -EINVAL;
	}

	mutex_lock(&tc6->spi_ctrl_lock);
	ret = oa_tc6_perform_ctrl(tc6, address, value, length,
				  OA_TC6_CTRL_REG_READ);
	mutex_unlock(&tc6->spi_ctrl_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(oa_tc6_read_registers);

/**
 * oa_tc6_read_register - function for reading a MAC-PHY register.
 * @tc6: oa_tc6 struct.
 * @address: register address of the MAC-PHY to be read.
 * @value: value read from the @address register address of the MAC-PHY.
 *
 * Return: 0 on success otherwise failed.
 */
int oa_tc6_read_register(struct oa_tc6 *tc6, u32 address, u32 *value)
{
	return oa_tc6_read_registers(tc6, address, value, 1);
}
EXPORT_SYMBOL_GPL(oa_tc6_read_register);

/**
 * oa_tc6_write_registers - function for writing multiple consecutive registers.
 * @tc6: oa_tc6 struct.
 * @address: address of the first register to be written in the MAC-PHY.
 * @value: values to be written from the starting register address @address.
 * @length: number of consecutive registers to be written from @address.
 *
 * Maximum of 128 consecutive registers can be written starting at @address.
 *
 * Return: 0 on success otherwise failed.
 */
int oa_tc6_write_registers(struct oa_tc6 *tc6, u32 address, u32 value[],
			   u8 length)
{
	int ret;

	if (!length || length > OA_TC6_CTRL_MAX_REGISTERS) {
		dev_err(&tc6->spi->dev, "Invalid register length parameter\n");
		return -EINVAL;
	}

	mutex_lock(&tc6->spi_ctrl_lock);
	ret = oa_tc6_perform_ctrl(tc6, address, value, length,
				  OA_TC6_CTRL_REG_WRITE);
	mutex_unlock(&tc6->spi_ctrl_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(oa_tc6_write_registers);

/**
 * oa_tc6_write_register - function for writing a MAC-PHY register.
 * @tc6: oa_tc6 struct.
 * @address: register address of the MAC-PHY to be written.
 * @value: value to be written in the @address register address of the MAC-PHY.
 *
 * Return: 0 on success otherwise failed.
 */
int oa_tc6_write_register(struct oa_tc6 *tc6, u32 address, u32 value)
{
	return oa_tc6_write_registers(tc6, address, &value, 1);
}
EXPORT_SYMBOL_GPL(oa_tc6_write_register);

static int oa_tc6_check_phy_reg_direct_access_capability(struct oa_tc6 *tc6)
{
	u32 regval;
	int ret;

	ret = oa_tc6_read_register(tc6, OA_TC6_REG_STDCAP, &regval);
	if (ret)
		return ret;

	if (!(regval & STDCAP_DIRECT_PHY_REG_ACCESS))
		return -ENODEV;

	return 0;
}

static void oa_tc6_handle_link_change(struct net_device *netdev)
{
	phy_print_status(netdev->phydev);
}

static int oa_tc6_mdiobus_read(struct mii_bus *bus, int addr, int regnum)
{
	struct oa_tc6 *tc6 = bus->priv;
	u32 regval;
	int ret;

	ret = oa_tc6_read_register(tc6,
				   OA_TC6_PHY_STD_REG_ADDR_BASE |
					   (regnum &
					    OA_TC6_PHY_STD_REG_ADDR_MASK),
				   &regval);
	if (ret)
		return ret;

	return regval;
}

static int oa_tc6_mdiobus_write(struct mii_bus *bus, int addr, int regnum,
				u16 val)
{
	struct oa_tc6 *tc6 = bus->priv;

	return oa_tc6_write_register(tc6,
				     OA_TC6_PHY_STD_REG_ADDR_BASE |
					     (regnum &
					      OA_TC6_PHY_STD_REG_ADDR_MASK),
				     val);
}

static int oa_tc6_get_phy_c45_mms(int devnum)
{
#ifndef MDIO_MMD_POWER_UNIT
#define MDIO_MMD_POWER_UNIT 13
#endif
	switch (devnum) {
	case MDIO_MMD_PCS:
		return OA_TC6_PHY_C45_PCS_MMS2;
	case MDIO_MMD_PMAPMD:
		return OA_TC6_PHY_C45_PMA_PMD_MMS3;
	case MDIO_MMD_VEND2:
		return OA_TC6_PHY_C45_VS_PLCA_MMS4;
	case MDIO_MMD_AN:
		return OA_TC6_PHY_C45_AUTO_NEG_MMS5;
	case MDIO_MMD_POWER_UNIT:
		return OA_TC6_PHY_C45_POWER_UNIT_MMS6;
	default:
		return -EOPNOTSUPP;
	}
}

static int oa_tc6_mdiobus_read_c45(struct mii_bus *bus, int addr, int devnum,
				   int regnum)
{
	struct oa_tc6 *tc6 = bus->priv;
	u32 regval;
	int ret;

	ret = oa_tc6_get_phy_c45_mms(devnum);
	if (ret < 0)
		return ret;

	ret = oa_tc6_read_register(tc6, (ret << 16) | regnum, &regval);
	if (ret)
		return ret;

	return regval;
}

static int oa_tc6_mdiobus_write_c45(struct mii_bus *bus, int addr, int devnum,
				    int regnum, u16 val)
{
	struct oa_tc6 *tc6 = bus->priv;
	int ret;

	ret = oa_tc6_get_phy_c45_mms(devnum);
	if (ret < 0)
		return ret;

	return oa_tc6_write_register(tc6, (ret << 16) | regnum, val);
}

static int oa_tc6_mdiobus_register(struct oa_tc6 *tc6)
{
	int ret;

	tc6->mdiobus = mdiobus_alloc();
	if (!tc6->mdiobus) {
		netdev_err(tc6->netdev, "MDIO bus alloc failed\n");
		return -ENOMEM;
	}

	tc6->mdiobus->priv = tc6;
	tc6->mdiobus->read = oa_tc6_mdiobus_read;
	tc6->mdiobus->write = oa_tc6_mdiobus_write;
	/* OPEN Alliance 10BASE-T1x compliance MAC-PHYs will have both C22 and
	 * C45 registers space. If the PHY is discovered via C22 bus protocol it
	 * assumes it uses C22 protocol and always uses C22 registers indirect
	 * access to access C45 registers. This is because, we don't have a
	 * clean separation between C22/C45 register space and C22/C45 MDIO bus
	 * protocols. Resulting, PHY C45 registers direct access can't be used
	 * which can save multiple SPI bus access. To support this feature, PHY
	 * drivers can set .read_mmd/.write_mmd in the PHY driver to call
	 * .read_c45/.write_c45. Ex: drivers/net/phy/microchip_t1s.c
	 */
	tc6->mdiobus->read_c45 = oa_tc6_mdiobus_read_c45;
	tc6->mdiobus->write_c45 = oa_tc6_mdiobus_write_c45;
	tc6->mdiobus->name = "oa-tc6-mdiobus";
	tc6->mdiobus->parent = &tc6->spi->dev;

	snprintf(tc6->mdiobus->id, ARRAY_SIZE(tc6->mdiobus->id), "%s",
		 dev_name(&tc6->spi->dev));

	ret = mdiobus_register(tc6->mdiobus);
	if (ret) {
		netdev_err(tc6->netdev, "Could not register MDIO bus\n");
		mdiobus_free(tc6->mdiobus);
		return ret;
	}

	return 0;
}

static void oa_tc6_mdiobus_unregister(struct oa_tc6 *tc6)
{
	mdiobus_unregister(tc6->mdiobus);
	mdiobus_free(tc6->mdiobus);
}

static int oa_tc6_phy_init(struct oa_tc6 *tc6)
{
	int ret;

	ret = oa_tc6_check_phy_reg_direct_access_capability(tc6);
	if (ret) {
		netdev_err(
			tc6->netdev,
			"Direct PHY register access is not supported by the MAC-PHY\n");
		return ret;
	}

	ret = oa_tc6_mdiobus_register(tc6);
	if (ret)
		return ret;

	tc6->phydev = phy_find_first(tc6->mdiobus);
	if (!tc6->phydev) {
		netdev_err(tc6->netdev, "No PHY found\n");
		oa_tc6_mdiobus_unregister(tc6);
		return -ENODEV;
	}

	tc6->phydev->is_internal = true;
	ret = phy_connect_direct(tc6->netdev, tc6->phydev,
				 &oa_tc6_handle_link_change,
				 PHY_INTERFACE_MODE_INTERNAL);
	if (ret) {
		netdev_err(tc6->netdev, "Can't attach PHY to %s\n",
			   tc6->mdiobus->id);
		oa_tc6_mdiobus_unregister(tc6);
		return ret;
	}

	phy_attached_info(tc6->netdev->phydev);

	return 0;
}

static void oa_tc6_phy_exit(struct oa_tc6 *tc6)
{
	phy_disconnect(tc6->phydev);
	oa_tc6_mdiobus_unregister(tc6);
}

static int oa_tc6_read_status0(struct oa_tc6 *tc6)
{
	u32 regval;
	int ret;

	ret = oa_tc6_read_register(tc6, OA_TC6_REG_STATUS0, &regval);
	if (ret) {
		dev_err(&tc6->spi->dev, "STATUS0 register read failed: %d\n",
			ret);
		return 0;
	}

	return regval;
}

static int oa_tc6_sw_reset_macphy(struct oa_tc6 *tc6)
{
	u32 st0;
#if defined(CONFIG_RPI4)
	u32 regval = RESET_SWRESET;
	int ret;
#elif defined(CONFIG_RPI5)
	u32 regval;
	int ret;

	oa_tc6_read_register(tc6, OA_TC6_REG_STATUS0, &regval);
	pr_info("oa_tc6_sw_reset_macphy: STATUS0=0x%08x\n", regval);
	regval = RESET_SWRESET;
#endif /* CONFIG_RPI4 or CONFIG_RPI5 */

	/* TC6 v1.1 9.2.4.2 / 9.2.8.8: RESETC stays set until the host writes
	 * a 1 to it.  Clear one still set from an earlier reset (power-on,
	 * RESET_N, a previous SWRESET) first, so that the RESETC polled below
	 * can only come from this software reset - otherwise a MAC-PHY that
	 * takes longer to reset is configured while it is still resetting.
	 */
	if (!oa_tc6_read_register(tc6, OA_TC6_REG_STATUS0, &st0) &&
	    (st0 & STATUS0_RESETC))
		oa_tc6_write_register(tc6, OA_TC6_REG_STATUS0, STATUS0_RESETC);

	ret = oa_tc6_write_register(tc6, OA_TC6_REG_RESET, regval);
	if (ret) {
		pr_err("oa_tc6_sw_reset_macphy: write RESET reg failed (ret=%d)\n", ret);
		return ret;
	}
	pr_info("oa_tc6_sw_reset_macphy: wrote RESET=0x%08x\n", regval);

	/* Poll for soft reset complete for every 1ms until 1s timeout */
	ret = readx_poll_timeout(oa_tc6_read_status0, tc6, regval,
				 regval & STATUS0_RESETC,
				 STATUS0_RESETC_POLL_DELAY,
				 STATUS0_RESETC_POLL_TIMEOUT);
	if (ret) {
		pr_err("oa_tc6_sw_reset_macphy: reset complete timeout (last regval=0x%08x)\n",
		       regval);
		return -ENODEV;
	}
	pr_info("oa_tc6_sw_reset_macphy: reset complete, STATUS0=0x%08x\n", regval);

	/* Clear the reset complete status */
	ret = oa_tc6_write_register(tc6, OA_TC6_REG_STATUS0, regval);
	if (ret)
		pr_err("oa_tc6_sw_reset_macphy: clear STATUS0 failed (ret=%d)\n", ret);
	else
		pr_info("oa_tc6_sw_reset_macphy: cleared STATUS0=0x%08x\n", regval);

	return ret;
}

// static int oa_tc6_sw_reset_macphy(struct oa_tc6 *tc6)
// {
// 	u32 regval = RESET_SWRESET;
// 	int ret;

// 	ret = oa_tc6_write_register(tc6, OA_TC6_REG_RESET, regval);
// 	if (ret)
// 		return ret;

// 	/* Poll for soft reset complete for every 1ms until 1s timeout */
// 	ret = readx_poll_timeout(oa_tc6_read_status0, tc6, regval,
// 				 regval & STATUS0_RESETC,
// 				 STATUS0_RESETC_POLL_DELAY,
// 				 STATUS0_RESETC_POLL_TIMEOUT);
// 	if (ret)
// 		return -ENODEV;

// 	/* Clear the reset complete status */
// 	return oa_tc6_write_register(tc6, OA_TC6_REG_STATUS0, regval);
// }

/* Minimum CSn-high time between SPI transactions (TC6 v1.1 resync_timer,
 * 8.4: implementation defined).  200 ns covers the TSN Lab T1S MAC-PHY
 * (160 ns, tools/oaspi_drv/README.md in the t1s repository); raise it for a
 * MAC-PHY that needs more.
 */
static unsigned int cs_inactive_ns = 200;
module_param(cs_inactive_ns, uint, 0444);
MODULE_PARM_DESC(cs_inactive_ns, "Minimum CSn-high time between SPI transactions in ns (TC6 resync_timer), default 200");

/* A configured spi_delay in ns (0 for units that do not give a time) */
static u32 oa_tc6_cs_inactive_ns(const struct spi_delay *d)
{
	if (d->unit == SPI_DELAY_UNIT_NSECS)
		return d->value;
	if (d->unit == SPI_DELAY_UNIT_USECS)
		return d->value * 1000;
	return 0;
}

/* err_irq=1 (default): unmask the error sources below so that they set the
 * footer EXST bit and assert IRQ_N.  err_irq=0 keeps the SDK behaviour up to
 * dbaae29b (all error sources masked; errors pass silently).
 */
#ifndef LAN865X_DEF_ERR_IRQ
#define LAN865X_DEF_ERR_IRQ true
#endif
static bool err_irq = LAN865X_DEF_ERR_IRQ;
module_param(err_irq, bool, 0444);
MODULE_PARM_DESC(err_irq, "Unmask MAC-PHY error interrupts (OA_IMASK0/1), default 1");

const char oa_tc6_stat_names[OA_TC6_STAT_NUM][32] = {
	[OA_TC6_STAT_TXPE] = "oa_tx_protocol_errors",
	[OA_TC6_STAT_TXBOE] = "oa_tx_buffer_overflows",
	[OA_TC6_STAT_TXBUE] = "oa_tx_buffer_underflows",
	[OA_TC6_STAT_RXBOE] = "oa_rx_buffer_overflows",
	[OA_TC6_STAT_LOFE] = "oa_loss_of_framing",
	[OA_TC6_STAT_HDRE] = "oa_header_errors",
	[OA_TC6_STAT_RESETC] = "oa_unexpected_resets",
	[OA_TC6_STAT_TXFCSE] = "oa_tx_fcs_errors",
	[OA_TC6_STAT_CPDE] = "oa_ctrl_protection_errors",
	[OA_TC6_STAT_RXNER] = "oa_rx_nonrecoverable",
	[OA_TC6_STAT_TXNER] = "oa_tx_nonrecoverable",
	[OA_TC6_STAT_FSMSTER] = "oa_fsm_state_errors",
	[OA_TC6_STAT_ECC] = "oa_sram_ecc_errors",
	[OA_TC6_STAT_UV18] = "oa_uv18_events",
	[OA_TC6_STAT_BUSER] = "oa_bus_errors",
	[OA_TC6_STAT_FOOTER_HDRB] = "oa_footer_header_bad",
	[OA_TC6_STAT_FOOTER_UNSYNC] = "oa_footer_unsync",
	[OA_TC6_STAT_FATAL] = "oa_fatal_errors",
	[OA_TC6_STAT_FOOTER_PARITY] = "oa_footer_parity_errors",
};
EXPORT_SYMBOL_GPL(oa_tc6_stat_names);

void oa_tc6_get_stats(struct oa_tc6 *tc6, u64 *data)
{
	memcpy(data, tc6->stats, sizeof(tc6->stats));
}
EXPORT_SYMBOL_GPL(oa_tc6_get_stats);

/* LOFE (loss of framing, early CS_N deassertion) is kept masked by default:
 * on the lab Raspberry Pi 4 (spi-bcm2835, 25 MHz, SPI mode 3) it is set
 * during the probe's control transactions and re-asserts immediately after
 * every W1C, while all control echoes and data chunks are intact.  Unmasked,
 * it made the first data transfer fail (traffic disabled at probe).  This is
 * presumably why the SDK put the unmask under '#if 0'.  TODO(spec): check the
 * CS_N timing against DS60001734F (SPI timing) with a logic analyser.
 */
#ifndef LAN865X_DEF_LOFE_IRQ
#define LAN865X_DEF_LOFE_IRQ false
#endif
static bool lofe_irq = LAN865X_DEF_LOFE_IRQ;
module_param(lofe_irq, bool, 0444);
MODULE_PARM_DESC(lofe_irq, "Also unmask LOFE (loss of framing), default 0");

static int oa_tc6_unmask_macphy_error_interrupts(struct oa_tc6 *tc6)
{
	u32 regval;
	int ret;

	ret = oa_tc6_read_register(tc6, OA_TC6_REG_INT_MASK0, &regval);
	if (ret)
		return ret;

	/* The SDK had this unmask under '#if 0' (20250317), so OA_IMASK0
	 * kept its reset value (all error sources masked, DS60001734F
	 * 11.1.9) and TXPE/TXBOE/RXBOE/LOFE/HDRE never reached the host.
	 * TXBOE/TXBUE are unmasked in addition to the upstream set: a TX
	 * buffer overflow means the host overran the chip's credits, which
	 * is exactly what has to be visible while chasing TX stalls.
	 */
	if (err_irq)
		regval &= ~(INT_MASK0_TX_PROTOCOL_ERR_MASK |
			    INT_MASK0_TX_BUFFER_OVERFLOW_ERR_MASK |
			    INT_MASK0_TX_BUFFER_UNDERFLOW_ERR_MASK |
			    INT_MASK0_RX_BUFFER_OVERFLOW_ERR_MASK |
			    INT_MASK0_HEADER_ERR_MASK);
	if (err_irq && lofe_irq)
		regval &= ~INT_MASK0_LOSS_OF_FRAME_ERR_MASK;

	ret = oa_tc6_write_register(tc6, OA_TC6_REG_INT_MASK0, regval);
	if (ret || !err_irq)
		return ret;

	/* DS60001734F 4.6.2: on TXNER/RXNER the transfer of Ethernet frames
	 * through the device is halted and "the host controller must perform
	 * a hardware or software reset"; the same applies to FSMSTER and
	 * BUSER.  All of them are masked at reset (OA_IMASK1 reset value).
	 */
	ret = oa_tc6_read_register(tc6, OA_TC6_REG_INT_MASK1, &regval);
	if (ret)
		return ret;

	regval &= ~(INT_MASK1_RXNERM | INT_MASK1_TXNERM | INT_MASK1_FSMSTERM |
		    INT_MASK1_ECCM | INT_MASK1_BUSERM);

	return oa_tc6_write_register(tc6, OA_TC6_REG_INT_MASK1, regval);
}

static int oa_tc6_enable_data_transfer(struct oa_tc6 *tc6)
{
	u32 value;
	int ret;

	ret = oa_tc6_read_register(tc6, OA_TC6_REG_CONFIG0, &value);
	if (ret)
		return ret;

	/* Enable configuration synchronization for data transfer */
	value |= CONFIG0_SYNC;

	return oa_tc6_write_register(tc6, OA_TC6_REG_CONFIG0, value);
}

/* Called when a frame that is meant to be transmitted, is dropped. */
static void oa_tc6_drop_tx_skb(struct oa_tc6 *tc6, struct sk_buff *skb)
{
	if (skb) {
		tc6->netdev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
	}
}

static struct sk_buff *oa_tc6_detach_waiting_tx_skb(struct oa_tc6 *tc6)
{
	struct sk_buff *skb;

	lockdep_assert_held(&tc6->tx_skb_lock);
	skb = tc6->waiting_tx_skb;
	tc6->waiting_tx_skb = NULL;
#ifdef FRAME_TIMESTAMP_ENABLE
	tc6->waiting_tx_ts_capture_mode = 0;
#endif /* FRAME_TIMESTAMP_ENABLE */

	return skb;
}

static void oa_tc6_cleanup_ongoing_rx_skb(struct oa_tc6 *tc6)
{
	if (tc6->rx_skb) {
		tc6->netdev->stats.rx_dropped++;
		kfree_skb(tc6->rx_skb);
		tc6->rx_skb = NULL;
	}
}

#ifdef FRAME_TIMESTAMP_ENABLE
/* Filter RX timestamp based on hwtstamp_config */
static bool filter_rx_timestamp(struct oa_tc6 *tc6, u8 *data)
{
	struct net_device *netdev = tc6->netdev;
	struct lan865x_priv *priv = netdev_priv(netdev);
	int rx_filter = priv->tstamp_config.rx_filter;
	struct ethhdr *eth;
	u8 *payload = data;
	u16 eth_type;
	struct vlan_hdr *vlan;
	struct ptp_header *ptp;
	u8 msg_type;

	if (rx_filter == HWTSTAMP_FILTER_NONE) {
		return false;
	} else if (rx_filter == HWTSTAMP_FILTER_ALL) {
		return true;
	}

	eth = (struct ethhdr *)payload;
	payload += sizeof(*eth);
	eth_type = ntohs(eth->h_proto);
	if (eth_type == ETH_P_8021Q || eth_type == ETH_P_8021AD) {
		vlan = (struct vlan_hdr *)payload;
		eth_type = ntohs(vlan->h_vlan_encapsulated_proto);
		payload += sizeof(*vlan);
	}

	if (eth_type != ETH_P_1588) {
		return false;
	}

	ptp = (struct ptp_header *)payload;
	msg_type = ptp->tsmt & 0xF;
	switch (rx_filter) {
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
		return true;
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
		return msg_type == PTP_MSGTYPE_SYNC;
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
	case HWTSTAMP_FILTER_PTP_V2_L2_DELAY_REQ:
		return msg_type == PTP_MSGTYPE_DELAY_REQ;
	default:
		return false;
	}
}
#endif /* FRAME_TIMESTAMP_ENABLE */

static void oa_tc6_cleanup_ongoing_tx_skb(struct oa_tc6 *tc6)
{
	oa_tc6_drop_tx_skb(tc6, tc6->ongoing_tx_skb);
	tc6->ongoing_tx_skb = NULL;
#ifdef FRAME_TIMESTAMP_ENABLE
	tc6->ongoing_tx_ts_capture_mode = 0;
#endif /* FRAME_TIMESTAMP_ENABLE */
	tc6->tx_skb_offset = 0;
}

static void oa_tc6_cleanup_waiting_tx_skb(struct oa_tc6 *tc6)
{
	struct sk_buff *skb;

	spin_lock_bh(&tc6->tx_skb_lock);
	skb = oa_tc6_detach_waiting_tx_skb(tc6);
	spin_unlock_bh(&tc6->tx_skb_lock);

	oa_tc6_drop_tx_skb(tc6, skb);
}

static void oa_tc6_free_ongoing_skbs(struct oa_tc6 *tc6)
{
	oa_tc6_cleanup_ongoing_tx_skb(tc6);
	oa_tc6_cleanup_ongoing_rx_skb(tc6);
}

static void oa_tc6_free_pending_skbs(struct oa_tc6 *tc6)
{
	oa_tc6_free_ongoing_skbs(tc6);
	oa_tc6_cleanup_waiting_tx_skb(tc6);
}

#define OA_TC6_IRQ_OFF 0

/* Disable the IRQ once on a fatal error.  Upstream calls disable_irq_nosync()
 * from both the hard and the threaded handler and never re-enables it, so
 * the depth does not matter there; oa_tc6_start_traffic() has to undo it
 * exactly once.
 */
static void oa_tc6_fatal_irq_off(struct oa_tc6 *tc6)
{
	if (!test_and_set_bit(OA_TC6_IRQ_OFF, &tc6->irq_flags))
		disable_irq_nosync(tc6->spi->irq);
}

/* If the failure is at SPI interface level, masking and clearing
 * the interrupt of the device won't work. Since SPI interrupt is
 * disabled, it should stop the repeated interrupts.
 */
static void oa_tc6_disable_traffic(struct oa_tc6 *tc6)
{
	u32 regval = INT_MASK0_ALL_INTERRUPTS;
	struct sk_buff *skb;

	tc6->stats[OA_TC6_STAT_FATAL]++;
	spin_lock_bh(&tc6->tx_skb_lock);
	tc6->disable_traffic = true;
	skb = oa_tc6_detach_waiting_tx_skb(tc6);
	spin_unlock_bh(&tc6->tx_skb_lock);

	/* disable_traffic, when set, is a point of no return to
	 * working state. Keeping the TX queues disabled.
	 */
	netif_tx_disable(tc6->netdev);
	oa_tc6_drop_tx_skb(tc6, skb);
	oa_tc6_free_ongoing_skbs(tc6);
	oa_tc6_write_register(tc6, OA_TC6_REG_INT_MASK0, regval);
	oa_tc6_read_register(tc6, OA_TC6_REG_STATUS0, &regval);
	oa_tc6_write_register(tc6, OA_TC6_REG_STATUS0, regval);
	dev_err(&tc6->spi->dev, "Device interrupt disabled to avoid interrupt storm");
}

static void oa_tc6_look_for_new_frame(struct oa_tc6 *tc6)
{
	tc6->wait_until_start_valid = true;
	oa_tc6_cleanup_ongoing_rx_skb(tc6);
}

/* STATUS0/STATUS1 bits owned by this function.  Only these are written back
 * (W1C).  The SDK wrote back the whole register, which also cleared the
 * TTSCAA/B/C (and STATUS1 TTSCMx/TTSCOFx) bits that the PTP thread polls,
 * and RESETC.
 */
#define STATUS0_HANDLED (STATUS0_TX_PROTOCOL_ERROR | \
			 STATUS0_TX_BUFFER_OVERFLOW_ERROR | \
			 STATUS0_TX_BUFFER_UNDERFLOW_ERROR | \
			 STATUS0_RX_BUFFER_OVERFLOW_ERROR | \
			 STATUS0_LOSS_OF_FRAME_ERROR | STATUS0_HEADER_ERROR | \
			 STATUS0_RESETC | STATUS0_TXFCSE | STATUS0_CPDE)
#define STATUS1_HANDLED (STATUS1_RXNER | STATUS1_TXNER | STATUS1_FSMSTER | \
			 STATUS1_ECC | STATUS1_UV18 | STATUS1_BUSER)

static void oa_tc6_count_status(struct oa_tc6 *tc6, u32 st0, u32 st1)
{
	static const struct {
		u32 bit;
		u8 reg;
		u8 id;
	} map[] = {
		{ STATUS0_TX_PROTOCOL_ERROR, 0, OA_TC6_STAT_TXPE },
		{ STATUS0_TX_BUFFER_OVERFLOW_ERROR, 0, OA_TC6_STAT_TXBOE },
		{ STATUS0_TX_BUFFER_UNDERFLOW_ERROR, 0, OA_TC6_STAT_TXBUE },
		{ STATUS0_RX_BUFFER_OVERFLOW_ERROR, 0, OA_TC6_STAT_RXBOE },
		{ STATUS0_LOSS_OF_FRAME_ERROR, 0, OA_TC6_STAT_LOFE },
		{ STATUS0_HEADER_ERROR, 0, OA_TC6_STAT_HDRE },
		{ STATUS0_RESETC, 0, OA_TC6_STAT_RESETC },
		{ STATUS0_TXFCSE, 0, OA_TC6_STAT_TXFCSE },
		{ STATUS0_CPDE, 0, OA_TC6_STAT_CPDE },
		{ STATUS1_RXNER, 1, OA_TC6_STAT_RXNER },
		{ STATUS1_TXNER, 1, OA_TC6_STAT_TXNER },
		{ STATUS1_FSMSTER, 1, OA_TC6_STAT_FSMSTER },
		{ STATUS1_ECC, 1, OA_TC6_STAT_ECC },
		{ STATUS1_UV18, 1, OA_TC6_STAT_UV18 },
		{ STATUS1_BUSER, 1, OA_TC6_STAT_BUSER },
	};

	for (int i = 0; i < ARRAY_SIZE(map); i++)
		if ((map[i].reg ? st1 : st0) & map[i].bit)
			tc6->stats[map[i].id]++;
}

static int oa_tc6_process_extended_status(struct oa_tc6 *tc6)
{
	u32 st[2]; /* OA_STATUS0, OA_STATUS1: consecutive, one SPI transaction */
	u32 value, st1;
	int ret;

	ret = oa_tc6_read_registers(tc6, OA_TC6_REG_STATUS0, st, 2);
	if (ret) {
		netdev_err(tc6->netdev, "STATUS0/1 register read failed: %d\n",
			   ret);
		return ret;
	}
	value = st[0] & STATUS0_HANDLED;
	st1 = st[1] & STATUS1_HANDLED;

	/* This function is called for each chunk received in a given SPI
	 * transaction. In case, extended status bit is set in more than
	 * one chunk, skip the write, if status0 is already cleared.
	 */
	if (!value && !st1)
		return 0;

	oa_tc6_count_status(tc6, value, st1);

	/* Clear the error interrupts status */
	if (value) {
		ret = oa_tc6_write_register(tc6, OA_TC6_REG_STATUS0, value);
		if (ret) {
			netdev_err(tc6->netdev,
				   "STATUS0 register write failed: %d\n", ret);
			return ret;
		}
	}
	if (st1) {
		ret = oa_tc6_write_register(tc6, OA_TC6_REG_STATUS1, st1);
		if (ret) {
			netdev_err(tc6->netdev,
				   "STATUS1 register write failed: %d\n", ret);
			return ret;
		}
	}

	/* DS60001734F 4.6.2 / 11.1.7: the host must reset the device */
	if (st1 & (STATUS1_TXNER | STATUS1_RXNER | STATUS1_FSMSTER |
		   STATUS1_BUSER)) {
		netdev_err(tc6->netdev,
			   "Non-recoverable MAC-PHY error, STATUS1=0x%08x (TXNER=%d RXNER=%d FSMSTER=%d BUSER=%d)\n",
			   st[1], !!(st1 & STATUS1_TXNER),
			   !!(st1 & STATUS1_RXNER), !!(st1 & STATUS1_FSMSTER),
			   !!(st1 & STATUS1_BUSER));
		return -ENODEV;
	}
	if (st1 & (STATUS1_ECC | STATUS1_UV18))
		net_warn_ratelimited("%s: STATUS1=0x%08x (ECC=%d UV18=%d)\n",
				     tc6->netdev->name, st[1],
				     !!(st1 & STATUS1_ECC),
				     !!(st1 & STATUS1_UV18));

	/* RESETC outside of oa_tc6_sw_reset_macphy(): the device was reset
	 * behind our back (power glitch, RESET_N) and lost its configuration.
	 */
	if (value & STATUS0_RESETC) {
		netdev_err(tc6->netdev, "Unexpected MAC-PHY reset (RESETC)\n");
		return -ENODEV;
	}

	if (value & (STATUS0_TX_BUFFER_OVERFLOW_ERROR |
		     STATUS0_TX_BUFFER_UNDERFLOW_ERROR | STATUS0_TXFCSE |
		     STATUS0_CPDE))
		net_err_ratelimited("%s: STATUS0=0x%08x (TXBOE=%d TXBUE=%d TXFCSE=%d CPDE=%d)\n",
				    tc6->netdev->name, st[0],
				    !!(value & STATUS0_TX_BUFFER_OVERFLOW_ERROR),
				    !!(value & STATUS0_TX_BUFFER_UNDERFLOW_ERROR),
				    !!(value & STATUS0_TXFCSE),
				    !!(value & STATUS0_CPDE));

	if (FIELD_GET(STATUS0_RX_BUFFER_OVERFLOW_ERROR, value)) {
		oa_tc6_look_for_new_frame(tc6);
		net_err_ratelimited("%s: Receive buffer overflow error\n",
				    tc6->netdev->name);
		return -EAGAIN;
	}
	if (FIELD_GET(STATUS0_TX_PROTOCOL_ERROR, value)) {
		netdev_err(tc6->netdev, "Transmit protocol error\n");
		return -ENODEV;
	}
	/* TODO: Currently loss of frame and header errors are treated as
	 * non-recoverable errors. They will be handled in the next version.
	 */
	/* Not fatal here (upstream: -ENODEV), see lofe_irq. */
	if (FIELD_GET(STATUS0_LOSS_OF_FRAME_ERROR, value))
		net_warn_ratelimited("%s: loss of framing (LOFE) seen\n",
				     tc6->netdev->name);
	if (FIELD_GET(STATUS0_HEADER_ERROR, value)) {
		netdev_err(tc6->netdev, "Header error\n");
		return -ENODEV;
	}

	return 0;
}

static int oa_tc6_process_rx_chunk_footer(struct oa_tc6 *tc6, u32 footer)
{
	int ret = 0;

	/* Process rx chunk footer for the following,
	 * 1. tx credits
	 * 2. errors if any from MAC-PHY
	 * 3. receive chunks available
	 */
	tc6->tx_credits = FIELD_GET(OA_TC6_DATA_FOOTER_TX_CREDITS, footer);
	tc6->rx_chunks_available =
		FIELD_GET(OA_TC6_DATA_FOOTER_RX_CHUNKS, footer);

	if (FIELD_GET(OA_TC6_DATA_FOOTER_EXTENDED_STS, footer)) {
		ret = oa_tc6_process_extended_status(tc6);
		/* EAGAIN error is recoverable. Move on to check
		 * HEADER and SYNC errors before returning.
		 */
		if (ret && ret != -EAGAIN)
			return ret;
	}

	/* TODO: Currently received header bad and configuration unsync errors
	 * are treated as non-recoverable errors. They will be handled in the
	 * next version.
	 */
	if (FIELD_GET(OA_TC6_DATA_FOOTER_RXD_HEADER_BAD, footer)) {
		tc6->stats[OA_TC6_STAT_FOOTER_HDRB]++;
		netdev_err(tc6->netdev, "Rxd header bad error\n");
		return -ENODEV;
	}

	if (!FIELD_GET(OA_TC6_DATA_FOOTER_CONFIG_SYNC, footer)) {
		tc6->stats[OA_TC6_STAT_FOOTER_UNSYNC]++;
		netdev_err(tc6->netdev, "Config unsync error\n");
		return -ENODEV;
	}

	return ret;
}

static void oa_tc6_submit_rx_skb(struct oa_tc6 *tc6)
{
	tc6->rx_skb->protocol = eth_type_trans(tc6->rx_skb, tc6->netdev);
	tc6->netdev->stats.rx_packets++;
	tc6->netdev->stats.rx_bytes += tc6->rx_skb->len;

	netif_rx(tc6->rx_skb);

	tc6->rx_skb = NULL;
}

/* On oversubscribed traffic condition, particularly with overwhelming rx
 * buffer overflow errors, there could be data chunk loss. If tail + length
 * goes beyond end pointer, that is an indication that the data chunk with
 * end_valid bit is lost. Time to look for a data chunk with start_valid bit.
 *
 * If rx_skb is NULL, it is time to start looking for data chunk with
 * start_bit.
 */
static int oa_tc6_update_rx_skb(struct oa_tc6 *tc6, u8 *payload, u8 length)
{
	if (!tc6->rx_skb ||
	    skb_tailroom(tc6->rx_skb) < length) {
		oa_tc6_look_for_new_frame(tc6);
		return -EAGAIN;
	}

	memcpy(skb_put(tc6->rx_skb, length), payload, length);
	return 0;
}

/* On overwhelming rx buffer overflow errors, due to data chunk loss, it is
 * possible that we get two data chunks with start_valid bit set, without
 * end_valid bit set in between. In this case, rx_skb would have a valid
 * buffer pointer. We should release, if a valid pointer is found before
 * allocating a new one.
 */
static int oa_tc6_allocate_rx_skb(struct oa_tc6 *tc6)
{
	oa_tc6_cleanup_ongoing_rx_skb(tc6);
	tc6->rx_skb = netdev_alloc_skb_ip_align(
		tc6->netdev, tc6->netdev->mtu + ETH_HLEN + ETH_FCS_LEN);
	if (!tc6->rx_skb) {
		tc6->netdev->stats.rx_dropped++;
		return -ENOMEM;
	}

	return 0;
}

#ifdef FRAME_TIMESTAMP_ENABLE
static void oa_tc6_set_rx_hwtstamp(struct oa_tc6 *tc6, u8 *payload)
{
	if (filter_rx_timestamp(tc6, &payload[OA_TC6_TIMESTAMP_SIZE])) {
		struct timespec64 timestamp;
		__be32 *net_timestamp = (__be32 *)payload;

		timestamp.tv_sec = (s64)ntohl(net_timestamp[0]);
		timestamp.tv_nsec = (long)ntohl(net_timestamp[1]);

		skb_hwtstamps(tc6->rx_skb)->hwtstamp = timespec64_to_ns(&timestamp);
	}
}
#endif /* FRAME_TIMESTAMP_ENABLE */

static int oa_tc6_prcs_complete_rx_frame(struct oa_tc6 *tc6, u8 *payload,
					 u16 size)
{
	int ret;

	ret = oa_tc6_allocate_rx_skb(tc6);
	if (ret)
		return ret;

#ifdef FRAME_TIMESTAMP_ENABLE
	oa_tc6_set_rx_hwtstamp(tc6, payload);
	ret = oa_tc6_update_rx_skb(tc6, &payload[OA_TC6_TIMESTAMP_SIZE], size - OA_TC6_TIMESTAMP_SIZE);
#else /* FRAME_TIMESTAMP_ENABLE */
	ret = oa_tc6_update_rx_skb(tc6, payload, size);
#endif /* FRAME_TIMESTAMP_ENABLE */
	if (ret)
		return ret;

	oa_tc6_submit_rx_skb(tc6);

	return 0;
}

static int oa_tc6_prcs_rx_frame_start(struct oa_tc6 *tc6, u8 *payload, u16 size)
{
	int ret;

	ret = oa_tc6_allocate_rx_skb(tc6);
	if (ret)
		return ret;

#ifdef FRAME_TIMESTAMP_ENABLE
	oa_tc6_set_rx_hwtstamp(tc6, payload);
	return oa_tc6_update_rx_skb(tc6, &payload[OA_TC6_TIMESTAMP_SIZE], size - OA_TC6_TIMESTAMP_SIZE);
#else /* FRAME_TIMESTAMP_ENABLE */
	return oa_tc6_update_rx_skb(tc6, payload, size);
#endif /* FRAME_TIMESTAMP_ENABLE */
}

static int oa_tc6_prcs_rx_frame_end(struct oa_tc6 *tc6, u8 *payload, u16 size)
{
	int ret;

#ifdef FRAME_TIMESTAMP_ENABLE
	/* NOTE: Remove unnecessary last 4 bytes. */
	ret = oa_tc6_update_rx_skb(tc6, payload, size - OA_TC6_UNNECESSARY_FOOTER_SIZE);
#else /* FRAME_TIMESTAMP_ENABLE */
	ret = oa_tc6_update_rx_skb(tc6, payload, size);
#endif /* FRAME_TIMESTAMP_ENABLE */
	if (!ret)
		oa_tc6_submit_rx_skb(tc6);
	return ret;
}

static int oa_tc6_prcs_ongoing_rx_frame(struct oa_tc6 *tc6, u8 *payload,
					u32 footer)
{
	return oa_tc6_update_rx_skb(tc6, payload, OA_TC6_CHUNK_PAYLOAD_SIZE);
}

static int oa_tc6_prcs_rx_chunk_payload(struct oa_tc6 *tc6, u8 *data,
					u32 footer)
{
	u8 start_byte_offset =
		FIELD_GET(OA_TC6_DATA_FOOTER_START_WORD_OFFSET, footer) *
		sizeof(u32);
	u8 end_byte_offset =
		FIELD_GET(OA_TC6_DATA_FOOTER_END_BYTE_OFFSET, footer);
	bool start_valid = FIELD_GET(OA_TC6_DATA_FOOTER_START_VALID, footer);
	bool end_valid = FIELD_GET(OA_TC6_DATA_FOOTER_END_VALID, footer);
	/* TC6 v1.1 7.3.7: FD (valid with EV only) - the frame ending in this
	 * chunk is to be dropped (cut-through frame the MAC rejected, or a
	 * frame the MAC-PHY terminated after a header error, 7.5.1).
	 */
	bool frame_drop = end_valid &&
			  FIELD_GET(OA_TC6_DATA_FOOTER_FRAME_DROP, footer);
	u16 size;

	/* Restart the new rx frame after receiving rx buffer overflow error */
	if (start_valid && tc6->wait_until_start_valid)
		tc6->wait_until_start_valid = false;

	if (tc6->wait_until_start_valid)
		return 0;

	/* Process the chunk with complete rx frame */
	if (start_valid && end_valid && start_byte_offset < end_byte_offset) {
		if (frame_drop) {
			oa_tc6_cleanup_ongoing_rx_skb(tc6);
			tc6->netdev->stats.rx_dropped++;
			return 0;
		}
		size = end_byte_offset + 1 - start_byte_offset;
		return oa_tc6_prcs_complete_rx_frame(
			tc6, &data[start_byte_offset], size);
	}

	/* Process the chunk with only rx frame start */
	if (start_valid && !end_valid) {
		size = OA_TC6_CHUNK_PAYLOAD_SIZE - start_byte_offset;
		return oa_tc6_prcs_rx_frame_start(tc6, &data[start_byte_offset],
						  size);
	}

	/* Process the chunk with only rx frame end */
	if (end_valid && !start_valid) {
		if (frame_drop) {
			oa_tc6_cleanup_ongoing_rx_skb(tc6);
			return 0;
		}
		size = end_byte_offset + 1;
		return oa_tc6_prcs_rx_frame_end(tc6, data, size);
	}

	/* Process the chunk with previous rx frame end and next rx frame
	 * start.
	 */
	if (start_valid && end_valid && start_byte_offset > end_byte_offset) {
		/* After rx buffer overflow error received, there might be a
		 * possibility of getting an end valid of a previously
		 * incomplete rx frame along with the new rx frame start valid.
		 */
		if (tc6->rx_skb && frame_drop) {
			oa_tc6_cleanup_ongoing_rx_skb(tc6);
		} else if (tc6->rx_skb) {
			size = end_byte_offset + 1;
			oa_tc6_prcs_rx_frame_end(tc6, data, size);

			/* Return value from oa_tc6_prcs_rx_frame_end is not
			 * checked. If it returned an error, it is to make
			 * the code to look for new frame. At this stage,
			 * code below is going to process a new frame. So,
			 * error condition is set to false, in case it is
			 * set before proceeding.
			 */
			tc6->wait_until_start_valid = false;
		}
		size = OA_TC6_CHUNK_PAYLOAD_SIZE - start_byte_offset;
		return oa_tc6_prcs_rx_frame_start(tc6, &data[start_byte_offset],
						  size);
	}

	/* Process the chunk with ongoing rx frame data */
	return oa_tc6_prcs_ongoing_rx_frame(tc6, data, footer);
}

static u32 oa_tc6_get_rx_chunk_footer(struct oa_tc6 *tc6, u16 footer_offset)
{
	u8 *rx_buf = tc6->spi_data_rx_buf;
	__be32 footer;

	footer = *((__be32 *)&rx_buf[footer_offset]);

	return be32_to_cpu(footer);
}

/* Consecutive receive footers with a parity error after which the SPI link
 * is taken as broken (stuck MISO, wrong SPI mode): fatal, the MAC driver
 * resets the MAC-PHY.
 */
#define OA_TC6_FOOTER_PARITY_ERR_MAX 8

static int oa_tc6_process_spi_data_rx_buf(struct oa_tc6 *tc6, u16 length)
{
	u16 no_of_rx_chunks = length / OA_TC6_CHUNK_SIZE;
	bool retry = false;
	int ret = 0;
	u32 footer;

	/* All the rx chunks in the receive SPI data buffer are examined here */
	for (int i = 0; i < no_of_rx_chunks; i++) {
		/* Last 4 bytes in each received chunk consist footer info */
		footer = oa_tc6_get_rx_chunk_footer(
			tc6, i * OA_TC6_CHUNK_SIZE + OA_TC6_CHUNK_PAYLOAD_SIZE);

		/* TC6 v1.1 7.3.7 / 8.5.2: the footer carries odd parity.  A
		 * footer with a parity error (bit error on MISO) has no field
		 * that can be trusted: do not take its TXC / RCA / status,
		 * drop the receive frame this chunk belongs to (wait for the
		 * next start of frame) and ask for a fresh footer with another
		 * (empty) data chunk.
		 */
		if (oa_tc6_get_parity(footer)) {
			tc6->stats[OA_TC6_STAT_FOOTER_PARITY]++;
			if (++tc6->footer_par_err_run >=
			    OA_TC6_FOOTER_PARITY_ERR_MAX) {
				netdev_err(tc6->netdev,
					   "%u receive footers in a row with a parity error\n",
					   tc6->footer_par_err_run);
				tc6->footer_par_err_run = 0;
				return -EIO;
			}
			tc6->tx_credits = 0;
			tc6->rx_chunks_available = 0;
			tc6->int_flag = true;
			oa_tc6_look_for_new_frame(tc6);
			retry = true;
			continue;
		}
		tc6->footer_par_err_run = 0;

		ret = oa_tc6_process_rx_chunk_footer(tc6, footer);
		if (ret) {
			if (ret != -EAGAIN)
				return ret;
			retry = true;
		}

		/* If there is a data valid chunks then process it for the
		 * information needed to determine the validity and the location
		 * of the receive frame data.
		 */
		if (FIELD_GET(OA_TC6_DATA_FOOTER_DATA_VALID, footer)) {
			u8 *payload =
				tc6->spi_data_rx_buf + i * OA_TC6_CHUNK_SIZE;

			ret = oa_tc6_prcs_rx_chunk_payload(tc6, payload,
							   footer);
			if (ret) {
				if (ret != -ENOMEM && ret != -EAGAIN)
					return ret;
				retry = true;
			}
		}
	}

	/* Not bailing out on recoverable error codes, -EAGAIN and
	 * -ENOMEM. If subsequent loop iterations, if any, succeeds,
	 * error code would be overwritten. retry flag helps to
	 * make the caller to continue and retry. Since recovery
	 * action for -ENOMEM and -EAGAIN are same, we are returning
	 * one of the error codes, that is -EAGAIN.
	 */
	if (retry) {
		ret = -EAGAIN;
		oa_tc6_look_for_new_frame(tc6);
	}

	return ret;
}

#ifdef FRAME_TIMESTAMP_ENABLE
static __be32 oa_tc6_prepare_data_header(bool data_valid, bool start_valid,
					 bool end_valid, u8 end_byte_offset, u8 ts_capture_mode)
#else /* FRAME_TIMESTAMP_ENABLE */
static __be32 oa_tc6_prepare_data_header(bool data_valid, bool start_valid,
					 bool end_valid, u8 end_byte_offset)
#endif /* FRAME_TIMESTAMP_ENABLE */
{
	u32 header =
		FIELD_PREP(OA_TC6_DATA_HEADER_DATA_NOT_CTRL,
			   OA_TC6_DATA_HEADER) |
		FIELD_PREP(OA_TC6_DATA_HEADER_DATA_VALID, data_valid) |
		FIELD_PREP(OA_TC6_DATA_HEADER_START_VALID, start_valid) |
		FIELD_PREP(OA_TC6_DATA_HEADER_END_VALID, end_valid) |
		FIELD_PREP(OA_TC6_DATA_HEADER_END_BYTE_OFFSET, end_byte_offset);
#ifdef FRAME_TIMESTAMP_ENABLE
	header |= FIELD_PREP(OA_TC6_DATA_HEADER_TIMESTAMP_CAPTURE, ts_capture_mode);
#endif /* FRAME_TIMESTAMP_ENABLE */

	header |= FIELD_PREP(OA_TC6_DATA_HEADER_PARITY,
			     oa_tc6_get_parity(header));

	return cpu_to_be32(header);
}

static void oa_tc6_add_tx_skb_to_spi_buf(struct oa_tc6 *tc6)
{
	enum oa_tc6_data_end_valid_info end_valid = OA_TC6_DATA_END_INVALID;
	__be32 *tx_buf = tc6->spi_data_tx_buf + tc6->spi_data_tx_buf_offset;
	u16 remaining_len = tc6->ongoing_tx_skb->len - tc6->tx_skb_offset;
	u8 *tx_skb_data = tc6->ongoing_tx_skb->data + tc6->tx_skb_offset;
	enum oa_tc6_data_start_valid_info start_valid;
	u8 end_byte_offset = 0;
	u16 length_to_copy;
#ifdef FRAME_TIMESTAMP_ENABLE
	u8 ts_capture_mode = 0;
#endif /* FRAME_TIMESTAMP_ENABLE */

	/* Initial value is assigned here to avoid more than 80 characters in
	 * the declaration place.
	 */
	start_valid = OA_TC6_DATA_START_INVALID;

	/* Set start valid if the current tx chunk contains the start of the tx
	 * ethernet frame.
	 */
	if (!tc6->tx_skb_offset)
		start_valid = OA_TC6_DATA_START_VALID;

	/* If the remaining tx skb length is more than the chunk payload size of
	 * 64 bytes then copy only 64 bytes and leave the ongoing tx skb for
	 * next tx chunk.
	 */
	length_to_copy = min_t(u16, remaining_len, OA_TC6_CHUNK_PAYLOAD_SIZE);

	/* Copy the tx skb data to the tx chunk payload buffer */
	memcpy(tx_buf + 1, tx_skb_data, length_to_copy);
	tc6->tx_skb_offset += length_to_copy;

	/* Set end valid if the current tx chunk contains the end of the tx
	 * ethernet frame.
	 */
	if (tc6->ongoing_tx_skb->len == tc6->tx_skb_offset) {
		end_valid = OA_TC6_DATA_END_VALID;
		end_byte_offset = length_to_copy - 1;
		tc6->tx_skb_offset = 0;
		tc6->netdev->stats.tx_bytes += tc6->ongoing_tx_skb->len;
		tc6->netdev->stats.tx_packets++;
		kfree_skb(tc6->ongoing_tx_skb);
		tc6->ongoing_tx_skb = NULL;
#ifdef FRAME_TIMESTAMP_ENABLE
		ts_capture_mode = tc6->ongoing_tx_ts_capture_mode;
		tc6->ongoing_tx_ts_capture_mode = 0;
#endif /* FRAME_TIMESTAMP_ENABLE */
	}

#ifdef FRAME_TIMESTAMP_ENABLE
	*tx_buf = oa_tc6_prepare_data_header(OA_TC6_DATA_VALID, start_valid,
					     end_valid, end_byte_offset, ts_capture_mode);
#else /* FRAME_TIMESTAMP_ENABLE */
	*tx_buf = oa_tc6_prepare_data_header(OA_TC6_DATA_VALID, start_valid,
					     end_valid, end_byte_offset);
#endif /* FRAME_TIMESTAMP_ENABLE */
	tc6->spi_data_tx_buf_offset += OA_TC6_CHUNK_SIZE;
}

static u16 oa_tc6_prepare_spi_tx_buf_for_tx_skbs(struct oa_tc6 *tc6)
{
	u16 used_tx_credits;

	/* Get tx skbs and convert them into tx chunks based on the tx credits
	 * available.
	 */
	for (used_tx_credits = 0; used_tx_credits < tc6->tx_credits;
	     used_tx_credits++) {
		if (!tc6->ongoing_tx_skb) {
			spin_lock_bh(&tc6->tx_skb_lock);
			tc6->ongoing_tx_skb = tc6->waiting_tx_skb;
			tc6->waiting_tx_skb = NULL;
#ifdef FRAME_TIMESTAMP_ENABLE
			tc6->ongoing_tx_ts_capture_mode = tc6->waiting_tx_ts_capture_mode;
			tc6->waiting_tx_ts_capture_mode = 0;
#endif /* FRAME_TIMESTAMP_ENABLE */
			spin_unlock_bh(&tc6->tx_skb_lock);
		}
		if (!tc6->ongoing_tx_skb)
			break;
		oa_tc6_add_tx_skb_to_spi_buf(tc6);
	}

	return used_tx_credits * OA_TC6_CHUNK_SIZE;
}

static void oa_tc6_add_empty_chunks_to_spi_buf(struct oa_tc6 *tc6,
					       u16 needed_empty_chunks)
{
	__be32 header;

#ifdef FRAME_TIMESTAMP_ENABLE
	header = oa_tc6_prepare_data_header(OA_TC6_DATA_INVALID,
					    OA_TC6_DATA_START_INVALID,
					    OA_TC6_DATA_END_INVALID, 0, 0);
#else /* FRAME_TIMESTAMP_ENABLE */
	header = oa_tc6_prepare_data_header(OA_TC6_DATA_INVALID,
					    OA_TC6_DATA_START_INVALID,
					    OA_TC6_DATA_END_INVALID, 0);
#endif /* FRAME_TIMESTAMP_ENABLE */

	while (needed_empty_chunks--) {
		__be32 *tx_buf =
			tc6->spi_data_tx_buf + tc6->spi_data_tx_buf_offset;

		*tx_buf = header;
		tc6->spi_data_tx_buf_offset += OA_TC6_CHUNK_SIZE;
	}
}

static u16 oa_tc6_prepare_spi_tx_buf_for_rx_chunks(struct oa_tc6 *tc6, u16 len)
{
	u16 tx_chunks = len / OA_TC6_CHUNK_SIZE;
	u16 needed_empty_chunks;

	/* If there are more chunks to receive than to transmit, we need to add
	 * enough empty tx chunks to allow the reception of the excess rx
	 * chunks.
	 */
	if (tx_chunks >= tc6->rx_chunks_available)
		return len;

	needed_empty_chunks = tc6->rx_chunks_available - tx_chunks;

	oa_tc6_add_empty_chunks_to_spi_buf(tc6, needed_empty_chunks);

	return needed_empty_chunks * OA_TC6_CHUNK_SIZE + len;
}

static int oa_tc6_try_spi_transfer(struct oa_tc6 *tc6)
{
	int ret;

	while (true) {
		u16 spi_len = 0;

		tc6->spi_data_tx_buf_offset = 0;

		if (tc6->ongoing_tx_skb || tc6->waiting_tx_skb)
			spi_len = oa_tc6_prepare_spi_tx_buf_for_tx_skbs(tc6);

		spi_len = oa_tc6_prepare_spi_tx_buf_for_rx_chunks(tc6, spi_len);

		if (tc6->int_flag) {
			tc6->int_flag = false;
			if (spi_len == 0) {
				oa_tc6_add_empty_chunks_to_spi_buf(tc6, 1);
				spi_len = OA_TC6_CHUNK_SIZE;
			}
		}

		if (spi_len == 0)
			break;

		ret = oa_tc6_spi_transfer(tc6, OA_TC6_DATA_HEADER, spi_len);
		if (ret) {
			netdev_err(tc6->netdev,
				   "SPI data transfer failed: %d\n", ret);
			return ret;
		}

		ret = oa_tc6_process_spi_data_rx_buf(tc6, spi_len);

		if (ret && ret != -EAGAIN) {
			oa_tc6_free_ongoing_skbs(tc6);
			netdev_err(tc6->netdev, "Device error: %d\n", ret);
			return ret;
		}

		if (!tc6->waiting_tx_skb && netif_queue_stopped(tc6->netdev))
			netif_wake_queue(tc6->netdev);
	}

	return 0;
}

static irqreturn_t oa_tc6_macphy_threaded_irq(int irq, void *data)
{
	struct oa_tc6 *tc6 = data;
	void (*cb)(void *ctx);
	int ret = 0;

	/* It is possible that interrupt woke the thread before it is
	 * disabled. Until we come up with good recovery mechanism,
	 * no need to attempt spi transfer, once it fails. Pending skbs
	 * are already freed.
	 */
	spin_lock_bh(&tc6->tx_skb_lock);
	if (tc6->disable_traffic || tc6->suspended) {
		spin_unlock_bh(&tc6->tx_skb_lock);
		return IRQ_HANDLED;
	}
	spin_unlock_bh(&tc6->tx_skb_lock);

	while (tc6->int_flag ||
	       (tc6->waiting_tx_skb && tc6->tx_credits)) {
		ret = oa_tc6_try_spi_transfer(tc6);
		if (ret) {
			oa_tc6_fatal_irq_off(tc6);
			oa_tc6_disable_traffic(tc6);
			/* Let the MAC driver reset the device (process context) */
			cb = READ_ONCE(tc6->fatal_cb);
			if (cb)
				cb(tc6->fatal_ctx);
			break;
		}
	}

	return IRQ_HANDLED;
}

static int oa_tc6_update_buffer_status_from_register(struct oa_tc6 *tc6)
{
	u32 value;
	int ret;

	/* Initially tx credits and rx chunks available to be updated from the
	 * register as there is no data transfer performed yet. Later they will
	 * be updated from the rx footer.
	 */
	ret = oa_tc6_read_register(tc6, OA_TC6_REG_BUFFER_STATUS, &value);
	if (ret)
		return ret;

	tc6->tx_credits = FIELD_GET(BUFFER_STATUS_TX_CREDITS_AVAILABLE, value);
	tc6->rx_chunks_available =
		FIELD_GET(BUFFER_STATUS_RX_CHUNKS_AVAILABLE, value);

	return 0;
}

static irqreturn_t oa_tc6_macphy_isr(int irq, void *data)
{
	struct oa_tc6 *tc6 = data;

	/* MAC-PHY interrupt can occur for the following reasons.
	 * - availability of tx credits if it was 0 before and not reported in
	 *   the previous rx footer.
	 * - availability of rx chunks if it was 0 before and not reported in
	 *   the previous rx footer.
	 * - extended status event not reported in the previous rx footer.
	 */
	if (tc6->disable_traffic)
		oa_tc6_fatal_irq_off(tc6);
	else
		tc6->int_flag = true;
	/* Wake IRQ thread to perform spi transfer . In case
	 * disable_traffic is set, threaded irq may run again
	 * one more time.
	 */
	return IRQ_WAKE_THREAD;
}

/**
 * oa_tc6_zero_align_receive_frame_enable - function to enable zero align
 * receive frame feature.
 * @tc6: oa_tc6 struct.
 *
 * Return: 0 on success otherwise failed.
 */
int oa_tc6_zero_align_receive_frame_enable(struct oa_tc6 *tc6)
{
	u32 regval;
	int ret;

	ret = oa_tc6_read_register(tc6, OA_TC6_REG_CONFIG0, &regval);
	if (ret)
		return ret;

	/* Set Zero-Align Receive Frame Enable */
	regval |= CONFIG0_ZARFE_ENABLE;

	return oa_tc6_write_register(tc6, OA_TC6_REG_CONFIG0, regval);
}
EXPORT_SYMBOL_GPL(oa_tc6_zero_align_receive_frame_enable);

/**
 * oa_tc6_start_xmit - function for sending the tx skb which consists ethernet
 * frame.
 * @tc6: oa_tc6 struct.
 * @skb: socket buffer in which the ethernet frame is stored.
 * @ts_capture_mode: timestamp capture mode (only used if FRAME_TIMESTAMP_ENABLE is defined).
 *
 * Return: NETDEV_TX_OK if the transmit ethernet frame skb added in the tx_skb_q
 * otherwise returns NETDEV_TX_BUSY.
 */
#ifdef FRAME_TIMESTAMP_ENABLE
netdev_tx_t oa_tc6_start_xmit(struct oa_tc6 *tc6, struct sk_buff *skb, u8 ts_capture_mode)
#else /* FRAME_TIMESTAMP_ENABLE */
netdev_tx_t oa_tc6_start_xmit(struct oa_tc6 *tc6, struct sk_buff *skb)
#endif /* FRAME_TIMESTAMP_ENABLE */
{
	if (skb_linearize(skb)) {
		oa_tc6_drop_tx_skb(tc6, skb);
		return NETDEV_TX_OK;
	}

	spin_lock_bh(&tc6->tx_skb_lock);
	if (tc6->waiting_tx_skb || tc6->suspended) {
		/* suspended: recovery in progress, the stack keeps the skb */
		netif_stop_queue(tc6->netdev);
		spin_unlock_bh(&tc6->tx_skb_lock);
		return NETDEV_TX_BUSY;
	}
	if (tc6->disable_traffic) {
		spin_unlock_bh(&tc6->tx_skb_lock);
		oa_tc6_drop_tx_skb(tc6, skb);
		return NETDEV_TX_OK;
	}
	tc6->waiting_tx_skb = skb;
#ifdef FRAME_TIMESTAMP_ENABLE
	tc6->waiting_tx_ts_capture_mode = ts_capture_mode;
#endif /* FRAME_TIMESTAMP_ENABLE */
	spin_unlock_bh(&tc6->tx_skb_lock);

	/* Wake the threaded IRQ to perform spi transfer. */
	irq_wake_thread(tc6->spi->irq, tc6);

	return NETDEV_TX_OK;
}
EXPORT_SYMBOL_GPL(oa_tc6_start_xmit);

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
	ret = oa_tc6_read_register(tc6, OA_TC6_REG_STATUS0, &regval);
	if (ret)
		return ret;

	/* Write 1 to RESETC bit of OA_STATUS0 */
	regval |= STATUS0_RESETC;
	ret = oa_tc6_write_register(tc6, OA_TC6_REG_STATUS0, regval);
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

/**
 * oa_tc6_init - allocates and initializes oa_tc6 structure.
 * @spi: device with which data will be exchanged.
 * @netdev: network device interface structure.
 *
 * Return: pointer reference to the oa_tc6 structure if the MAC-PHY
 * initialization is successful otherwise NULL.
 */
struct oa_tc6 *oa_tc6_init(struct spi_device *spi, struct net_device *netdev)
{
	return oa_tc6_init_chip(spi, netdev, init_lan865x);
}
EXPORT_SYMBOL_GPL(oa_tc6_init);

/**
 * oa_tc6_init_chip - oa_tc6_init() with a MAC-PHY specific init step.
 * @spi: device with which data will be exchanged.
 * @netdev: network device interface structure.
 * @chip_init: replaces init_lan865x() in the init and the reinit sequence
 *	(NULL = init_lan865x()).  Only TC6 standard steps stay common.
 *
 * Return: as oa_tc6_init().
 */
struct oa_tc6 *oa_tc6_init_chip(struct spi_device *spi,
				struct net_device *netdev,
				int (*chip_init)(struct oa_tc6 *tc6))
{
	struct oa_tc6 *tc6;
	int ret;

	tc6 = devm_kzalloc(&spi->dev, sizeof(*tc6), GFP_KERNEL);
	if (!tc6)
		return NULL;

	tc6->spi = spi;
	tc6->netdev = netdev;
	tc6->chip_init = chip_init ? chip_init : init_lan865x;
	SET_NETDEV_DEV(netdev, &spi->dev);
	mutex_init(&tc6->spi_ctrl_lock);
	spin_lock_init(&tc6->tx_skb_lock);

	/* Set the SPI controller to pump at realtime priority */
	tc6->spi->rt = true;

	/* OPEN Alliance TC6 v1.1 7.2, 7.5.2, 8.4: CSn has to stay deasserted
	 * for at least the MAC-PHY's resync_timer between transactions (data
	 * <-> control switches, and after a loss of framing the next
	 * transaction is jammed otherwise).  The value is implementation
	 * defined; ask the SPI core (spi_set_cs()) for a minimum CSn-high
	 * time instead of relying on the controller's own gap.  A longer
	 * delay given by DT / the MAC driver is kept.
	 */
	if (oa_tc6_cs_inactive_ns(&tc6->spi->cs_inactive) < cs_inactive_ns) {
		tc6->spi->cs_inactive.value = cs_inactive_ns;
		tc6->spi->cs_inactive.unit = SPI_DELAY_UNIT_NSECS;
	}
	if (spi_setup(tc6->spi) < 0)
		return NULL;

	tc6->spi_ctrl_tx_buf = devm_kzalloc(
		&tc6->spi->dev, OA_TC6_CTRL_SPI_BUF_SIZE, GFP_KERNEL);
	if (!tc6->spi_ctrl_tx_buf)
		return NULL;

	tc6->spi_ctrl_rx_buf = devm_kzalloc(
		&tc6->spi->dev, OA_TC6_CTRL_SPI_BUF_SIZE, GFP_KERNEL);
	if (!tc6->spi_ctrl_rx_buf)
		return NULL;

	tc6->spi_data_tx_buf = devm_kzalloc(
		&tc6->spi->dev, OA_TC6_SPI_DATA_BUF_SIZE, GFP_KERNEL);
	if (!tc6->spi_data_tx_buf)
		return NULL;

	tc6->spi_data_rx_buf = devm_kzalloc(
		&tc6->spi->dev, OA_TC6_SPI_DATA_BUF_SIZE, GFP_KERNEL);
	if (!tc6->spi_data_rx_buf)
		return NULL;

	ret = oa_tc6_sw_reset_macphy(tc6);
	if (ret) {
		dev_err(&tc6->spi->dev, "MAC-PHY software reset failed: %d\n",
			ret);
		return NULL;
	}

	ret = oa_tc6_unmask_macphy_error_interrupts(tc6);
	if (ret) {
		dev_err(&tc6->spi->dev,
			"MAC-PHY error interrupts unmask failed: %d\n", ret);
		return NULL;
	}

	ret = oa_tc6_phy_init(tc6);
	if (ret) {
		dev_err(&tc6->spi->dev,
			"MAC internal PHY initialization failed: %d\n", ret);
		return NULL;
	}

	ret = tc6->chip_init(tc6);
	if (ret) {
		dev_err(&tc6->spi->dev, "Failed to init_lan865x: %d\n", ret);
		goto phy_exit;
	}

	ret = oa_tc6_enable_data_transfer(tc6);
	if (ret) {
		dev_err(&tc6->spi->dev, "Failed to enable data transfer: %d\n",
			ret);
		goto phy_exit;
	}

	ret = oa_tc6_update_buffer_status_from_register(tc6);
	if (ret) {
		dev_err(&tc6->spi->dev, "Failed to update buffer status: %d\n",
			ret);
		goto phy_exit;
	}

	/* OPEN Alliance TC6 v1.1 7.7: IRQ_N is active low and level
	 * triggered.  The SDK overlay (lan8650-overlay-rpi4.dts) declares
	 * IRQ_TYPE_EDGE_FALLING; the trigger flags below override it.
	 */
	/* Requesting with IRQF_TRIGGER_LOW changes the trigger type of the
	 * existing (DT, edge) mapping.  It has to be put back on exit: the
	 * next probe maps the IRQ again from DT and of_irq_get() refuses a
	 * mapping whose type differs ("irq: type mismatch, failed to map
	 * hwirq-23"), so no driver could bind until a reboot (seen on the lab
	 * RPi4).  Hence request_threaded_irq() + free_irq() in oa_tc6_exit()
	 * instead of the devm variant.
	 */
	tc6->irq_type_orig = irq_get_trigger_type(tc6->spi->irq);
	ret = request_threaded_irq(tc6->spi->irq, oa_tc6_macphy_isr,
				   oa_tc6_macphy_threaded_irq,
				   IRQF_TRIGGER_LOW | IRQF_ONESHOT,
				   dev_name(&tc6->spi->dev), tc6);
	if (ret) {
		dev_err(&tc6->spi->dev, "Failed to request macphy isr %d\n",
			ret);
		goto phy_exit;
	}

	/* oa_tc6_sw_reset_macphy() function resets and clears the MAC-PHY reset
	 * complete status. IRQ is also asserted on reset completion and it is
	 * remain asserted until MAC-PHY receives a data chunk. So performing an
	 * empty data chunk transmission will deassert the IRQ. Refer section
	 * 7.7 and 9.2.8.8 in the OPEN Alliance specification for more details.
	 */
	tc6->int_flag = true;
	irq_wake_thread(tc6->spi->irq, tc6);

	return tc6;

phy_exit:
	oa_tc6_phy_exit(tc6);
	return NULL;
}
EXPORT_SYMBOL_GPL(oa_tc6_init_chip);

/**
 * oa_tc6_exit - exit function.
 * @tc6: oa_tc6 struct.
 */
void oa_tc6_exit(struct oa_tc6 *tc6)
{
	disable_irq(tc6->spi->irq);
	spin_lock_bh(&tc6->tx_skb_lock);
	tc6->disable_traffic = true;
	spin_unlock_bh(&tc6->tx_skb_lock);
	free_irq(tc6->spi->irq, tc6);
	if (tc6->irq_type_orig && tc6->irq_type_orig != IRQ_TYPE_LEVEL_LOW)
		irq_set_irq_type(tc6->spi->irq, tc6->irq_type_orig);
	oa_tc6_phy_exit(tc6);
	oa_tc6_free_pending_skbs(tc6);
}
EXPORT_SYMBOL_GPL(oa_tc6_exit);

/* ---- TX stall / fatal error recovery support (TSN Lab) ----
 *
 * None of this exists upstream: there, a fatal error leaves the device
 * dead until the driver is reloaded, and nothing detects a MAC-PHY that
 * silently stops returning TX credits (errata DS80001075F s4).
 */

/* The net_device given to oa_tc6_init_chip() (for the MAC driver's chip_init hook) */
struct net_device *oa_tc6_get_netdev(struct oa_tc6 *tc6)
{
	return tc6->netdev;
}
EXPORT_SYMBOL_GPL(oa_tc6_get_netdev);

void oa_tc6_set_fatal_cb(struct oa_tc6 *tc6, void (*cb)(void *ctx), void *ctx)
{
	tc6->fatal_ctx = ctx;
	tc6->fatal_cb = cb;
}
EXPORT_SYMBOL_GPL(oa_tc6_set_fatal_cb);

/* Snapshot for the stall detector.  Lockless reads of values written by the
 * IRQ thread; good enough for a check that has to hold for seconds.
 */
void oa_tc6_get_tx_state(struct oa_tc6 *tc6, struct oa_tc6_tx_state *st)
{
	st->tx_pending = READ_ONCE(tc6->waiting_tx_skb) ||
			 READ_ONCE(tc6->ongoing_tx_skb);
	st->tx_credits = READ_ONCE(tc6->tx_credits);
	st->disabled = READ_ONCE(tc6->disable_traffic);
}
EXPORT_SYMBOL_GPL(oa_tc6_get_tx_state);

/* OA_BUFSTS (MMS0 0x000B): TXC[15:8], RBA[7:0], DS60001734F 11.1.8 */
int oa_tc6_read_buffer_status(struct oa_tc6 *tc6, u8 *txc, u8 *rba)
{
	u32 value;
	int ret;

	ret = oa_tc6_read_register(tc6, OA_TC6_REG_BUFFER_STATUS, &value);
	if (ret)
		return ret;
	*txc = FIELD_GET(BUFFER_STATUS_TX_CREDITS_AVAILABLE, value);
	*rba = FIELD_GET(BUFFER_STATUS_RX_CHUNKS_AVAILABLE, value);
	return 0;
}
EXPORT_SYMBOL_GPL(oa_tc6_read_buffer_status);

/* Re-run a data transfer even though no interrupt came (lost IRQ). */
void oa_tc6_kick(struct oa_tc6 *tc6)
{
	tc6->int_flag = true;
	irq_wake_thread(tc6->spi->irq, tc6);
}
EXPORT_SYMBOL_GPL(oa_tc6_kick);

/* Park the data path: no SPI data transfer runs after this returns, and all
 * pending skbs are dropped.  Process context only (disable_irq() sleeps).
 */
void oa_tc6_stop_traffic(struct oa_tc6 *tc6)
{
	spin_lock_bh(&tc6->tx_skb_lock);
	tc6->suspended = true;
	spin_unlock_bh(&tc6->tx_skb_lock);

	/* Waits for a running IRQ thread; a thread woken later by
	 * irq_wake_thread() sees 'suspended' and returns.
	 */
	disable_irq(tc6->spi->irq);
	oa_tc6_free_pending_skbs(tc6);
}
EXPORT_SYMBOL_GPL(oa_tc6_stop_traffic);

/* Software-reset the MAC-PHY and redo the oa_tc6_init()/probe register
 * sequence (same order): OA_RESET.SWRESET, error IRQ unmask, AN1760
 * configuration + PLCA enable + MAC TX/RX enable (init_lan865x()), ZARFE,
 * CONFIG0.SYNC, buffer status.  The MAC driver restores its own settings
 * (MAC address, PLCA node ID, filters) afterwards.
 */
int oa_tc6_hw_reinit(struct oa_tc6 *tc6)
{
	int ret;

	ret = oa_tc6_sw_reset_macphy(tc6);
	if (ret)
		return ret;
	ret = oa_tc6_unmask_macphy_error_interrupts(tc6);
	if (ret)
		return ret;
	ret = tc6->chip_init(tc6);
	if (ret)
		return ret;
	ret = oa_tc6_zero_align_receive_frame_enable(tc6);
	if (ret)
		return ret;
	ret = oa_tc6_enable_data_transfer(tc6);
	if (ret)
		return ret;
	return oa_tc6_update_buffer_status_from_register(tc6);
}
EXPORT_SYMBOL_GPL(oa_tc6_hw_reinit);

/* Undo oa_tc6_stop_traffic() (and a fatal-error disable). */
void oa_tc6_start_traffic(struct oa_tc6 *tc6)
{
	tc6->tx_skb_offset = 0;
	tc6->wait_until_start_valid = false;
#ifdef FRAME_TIMESTAMP_ENABLE
	tc6->ongoing_tx_ts_capture_mode = 0;
#endif /* FRAME_TIMESTAMP_ENABLE */

	spin_lock_bh(&tc6->tx_skb_lock);
	tc6->suspended = false;
	tc6->disable_traffic = false;
	spin_unlock_bh(&tc6->tx_skb_lock);

	if (test_and_clear_bit(OA_TC6_IRQ_OFF, &tc6->irq_flags))
		enable_irq(tc6->spi->irq);
	enable_irq(tc6->spi->irq);

	/* IRQ_N is asserted after a reset until the first data chunk;
	 * an empty chunk also refreshes TXC/RCA (see oa_tc6_init()).
	 */
	oa_tc6_kick(tc6);
}
EXPORT_SYMBOL_GPL(oa_tc6_start_traffic);

MODULE_DESCRIPTION("OPEN Alliance 10BASE‑T1x MAC‑PHY Serial Interface Lib");
MODULE_AUTHOR("Parthiban Veerasooran <parthiban.veerasooran@microchip.com>");
MODULE_LICENSE("GPL");
