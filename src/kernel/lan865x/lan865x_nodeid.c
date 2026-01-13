#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/spinlock.h>

#include "lan865x_nodeid.h"

#define I2C_BUS_ADDRESS   1
#define FXL6408_I2C_ADDR  0x43
#define FXL6408_I2C_BUS   20
#define FXL6408_MF        0x05

static struct i2c_client  *fxl6408_client;
static DEFINE_SPINLOCK(i2c_lock);

u8 lan865x_nodeid_read_reg(u8 reg)
{
    u8  buf;

    spin_lock(&i2c_lock);
    i2c_master_send(fxl6408_client, &reg, 1);
    i2c_master_recv(fxl6408_client, &buf, 1);
    spin_unlock(&i2c_lock);
    return buf;
}

void lan865x_nodeid_write_reg(u8 reg, u8 val)
{
    spin_lock(&i2c_lock);
    i2c_master_send(fxl6408_client, &reg, 1);
    i2c_master_send(fxl6408_client, &val, 1);
    spin_unlock(&i2c_lock);
}

static DEFINE_SPINLOCK(recent_irq_lock);
static int recent_irq_flag = 0;
static struct task_struct *nodeid_thread;

static int get_nodeid(void* data)
{
    u8  val;

    msleep(20);
    val = lan865x_nodeid_read_reg(FXL6408_REG_INPUT_STATUS);
    lan865x_nodeid_write_reg(FXL6408_REG_INPUT_DEFAULT, val & 0xF0);
    pr_info("\nlan865x: fxl6408 input status      = 0x%02x\n\n", val);
    spin_lock_bh(&recent_irq_lock);
    recent_irq_flag = 0;
    spin_unlock_bh(&recent_irq_lock);
    return 0;
}

static irqreturn_t irq_handler(int irq, void* dev)
{
    u8  val;

    spin_lock_bh(&recent_irq_lock);
    if (recent_irq_flag == 0) {
        recent_irq_flag = 1;
        nodeid_thread = kthread_run(get_nodeid, NULL, "lan865x_nodeid_thread");
        if (nodeid_thread) {
            wake_up_process(nodeid_thread);
        } else {
            pr_warn("lan865x: failed to create kernel thread for node ID update\n");
        }
    }
    spin_unlock_bh(&recent_irq_lock);
    val = lan865x_nodeid_read_reg(FXL6408_REG_INTERRUPT_STATUS);
    pr_info("lan865x: fxl6408 interrupt status  = 0x%02x\n", val);
    return IRQ_HANDLED;
}

static void init_registers(void)
{
    struct fxl6408_device_id_ctrl {
        u8  SW_RST: 1;
        u8  RST_INT: 1;
        u8  FW_rev: 3;  // Firmware version
        u8  MF: 3;      // Manufacturer
    }   ctrl;
    u8  ret;

    ret = lan865x_nodeid_read_reg(FXL6408_REG_DEVICE_ID_CTRL);
    *(u8 *)&ctrl = ret;
    if (ctrl.MF != FXL6408_MF) {
        pr_warn("lan865x_nodeid: Manufacturer=0x%02x\n", ctrl.MF);
    }
    ctrl.SW_RST = 1;
    lan865x_nodeid_write_reg(FXL6408_REG_DEVICE_ID_CTRL, *(u8 *)&ctrl);
    lan865x_nodeid_write_reg(FXL6408_REG_OUTPUT_HIGH_Z, 0x00);
    lan865x_nodeid_write_reg(FXL6408_REG_IO_DIRECTION, 0x0F);
    lan865x_nodeid_write_reg(FXL6408_REG_INPUT_DEFAULT, 0xF0);
    lan865x_nodeid_write_reg(FXL6408_REG_INTERRUPT_MASK, 0x00);
    ret = lan865x_nodeid_read_reg(FXL6408_REG_DEVICE_ID_CTRL);
}

static int init_interrupt(struct device *dev)
{
    struct gpio_desc *gpiod;
    int irq;
    int ret;

    /* nodeid-gpios */
    gpiod = devm_gpiod_get(dev, "nodeid", GPIOD_IN);
    if (IS_ERR(gpiod)) {
      return PTR_ERR(gpiod);
    }
    irq = gpiod_to_irq(gpiod);
    if (irq < 0) {
      pr_warn("lan865x: failed to get IRQ number\n");
      return irq;
    }
    ret = devm_request_threaded_irq(dev, irq, NULL, irq_handler,
                             IRQF_TRIGGER_LOW | IRQF_ONESHOT,
                             "lan865x_irq",
                             dev);
    if (ret < 0) {
      pr_warn("lan865x: failed to allocate IRQ\n");
      return ret;
    }
    pr_info("lan865x: irq=%d\n", irq);
    return 0;
}

static struct i2c_board_info  fxl6408_info = {
    I2C_BOARD_INFO("fxl6408", FXL6408_I2C_ADDR)
};

int lan865x_nodeid_init(struct device *dev)
{
    struct i2c_adapter *adapter;
    int ret;

    adapter = i2c_get_adapter(I2C_BUS_ADDRESS);
    if (adapter == NULL) {
        pr_warn("lan865x_nodeid: failed to get i2c adapter\n");
        return -1;
    }
    fxl6408_client = i2c_new_client_device(adapter, &fxl6408_info);
    i2c_put_adapter(adapter);
    if (fxl6408_client == NULL) {
        pr_warn("lan865x_nodeid: failed to create i2c device\n");
        return -1;
    }
    pr_info("lan865x_nodeid: fxl6408 initialized\n");
    init_registers();
    ret = init_interrupt(dev);
    return ret;
}

void lan865x_nodeid_exit(void)
{
  i2c_unregister_device(fxl6408_client);
}
