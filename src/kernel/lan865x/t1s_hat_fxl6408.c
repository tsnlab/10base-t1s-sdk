#include "t1s_hat_fxl6408.h"

#include <linux/atomic/atomic-instrumented.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/semaphore.h>

#define I2C_BUS_ADDRESS 1
#define FXL6408_I2C_ADDR 0x43
#define FXL6408_I2C_BUS 20
#define FXL6408_MF 0x05
#define FXL6408_REG_SIZE 1

static struct i2c_client* fxl6408_client;
static struct task_struct* nodeid_thread;
static DEFINE_SEMAPHORE(nodeid_irq_sem, 0);

static atomic_t nodeid_irq_flag = ATOMIC_INIT(0);

u8 t1s_hat_fxl6408_read_reg(u8 reg) {
    struct i2c_msg msgs[2];
    u8 val = 0;
    int ret;

    msgs[0].addr = FXL6408_I2C_ADDR;
    msgs[0].flags = 0;
    msgs[0].len = FXL6408_REG_SIZE;
    msgs[0].buf = &reg;

    msgs[1].addr = FXL6408_I2C_ADDR;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len = FXL6408_REG_SIZE;
    msgs[1].buf = &val;

    ret = i2c_transfer(fxl6408_client->adapter, msgs, sizeof(msgs) / sizeof(struct i2c_msg));
    if (ret < 0) {
        pr_err("t1s_hat_fxl6408: i2c_transfer failed (ret=%d)\n", ret);
        return ret;
    }
    return val;
}

void t1s_hat_fxl6408_write_reg(u8 reg, u8 val) {
    struct i2c_msg msgs[2];
    int ret;

    msgs[0].addr = FXL6408_I2C_ADDR;
    msgs[0].flags = 0;
    msgs[0].len = FXL6408_REG_SIZE;
    msgs[0].buf = &reg;

    msgs[1].addr = FXL6408_I2C_ADDR;
    msgs[1].flags = 0;
    msgs[1].len = FXL6408_REG_SIZE;
    msgs[1].buf = &val;

    ret = i2c_transfer(fxl6408_client->adapter, msgs, sizeof(msgs) / sizeof(struct i2c_msg));
    if (ret < 0) {
        pr_err("t1s_hat_fxl6408: i2c_transfer failed (ret=%d)\n", ret);
    }
}

/*
 * Acquire node ID from FXL6408 input pin(4:7)
 *
 * Node ID selector can change signals of 4 pins
 * that are connected to the FXL6408.
 * Change on each pin can cause interrupt. Total 4 interrupts.
 * So there exists delay between first interrupt and last interrupt.
 * That's why msleep is used here.
 */
static int get_nodeid(void* data) {
    int ret;
    u8 val;

    while (1) {
        ret = down_interruptible(&nodeid_irq_sem);
        if (ret != 0) {
            pr_warn("t1s_hat_fxl6408: error while acquiring semaphore\n");
        }
        if (kthread_should_stop()) {
            break;
        }
        msleep(20);
        val = t1s_hat_fxl6408_read_reg(FXL6408_REG_INPUT_STATUS);
        t1s_hat_fxl6408_write_reg(FXL6408_REG_INPUT_DEFAULT, val);
        pr_info("t1s_hat_fxl6408: input status      = 0x%02x\n", val);
        val = t1s_hat_fxl6408_read_reg(FXL6408_REG_INPUT_DEFAULT);
        pr_info("t1s_hat_fxl6408: input default     = 0x%02x\n", val);
        pr_info("\n");
        atomic_set(&nodeid_irq_flag, 0);
    }
    return 0;
}

static irqreturn_t nodeid_threaded_irq(int irq, void* dev) {
    u8 val;

    if (atomic_cmpxchg(&nodeid_irq_flag, 0, 1) == 0) {
        up(&nodeid_irq_sem);
    }
    val = t1s_hat_fxl6408_read_reg(FXL6408_REG_INTERRUPT_STATUS);
    pr_info("t1s_hat_fxl6408: interrupt status  = 0x%02x\n", val);
    return IRQ_HANDLED;
}

static void init_registers(void) {
    struct fxl6408_device_id_ctrl {
        u8 SW_RST : 1;
        u8 RST_INT : 1;
        u8 FW_rev : 3; // Firmware version
        u8 MF : 3;     // Manufacturer
    } ctrl;
    u8 ret;

    ret = t1s_hat_fxl6408_read_reg(FXL6408_REG_DEVICE_ID_CTRL);
    *(u8*)&ctrl = ret;
    if (ctrl.MF != FXL6408_MF) {
        pr_warn("t1s_hat_fxl6408: Manufacturer=0x%02x\n", ctrl.MF);
    }
    ctrl.SW_RST = 1;
    t1s_hat_fxl6408_write_reg(FXL6408_REG_DEVICE_ID_CTRL, *(u8*)&ctrl);
    t1s_hat_fxl6408_write_reg(FXL6408_REG_OUTPUT_HIGH_Z, 0x00);
    t1s_hat_fxl6408_write_reg(FXL6408_REG_IO_DIRECTION, 0x0F);
    t1s_hat_fxl6408_write_reg(FXL6408_REG_INPUT_DEFAULT, 0x00);
    t1s_hat_fxl6408_write_reg(FXL6408_REG_INTERRUPT_MASK, 0x00);
    ret = t1s_hat_fxl6408_read_reg(FXL6408_REG_DEVICE_ID_CTRL);
}

static int init_interrupt(struct device* dev) {
    struct gpio_desc* gpiod;
    int irq;
    int ret;

    /* nodeid-gpios */
    gpiod = devm_gpiod_get(dev, "nodeid", GPIOD_IN);
    if (IS_ERR(gpiod)) {
        return PTR_ERR(gpiod);
    }
    irq = gpiod_to_irq(gpiod);
    if (irq < 0) {
        pr_warn("t1s_hat_fxl6408: failed to get IRQ number\n");
        return irq;
    }
    ret = devm_request_threaded_irq(dev, irq, NULL, nodeid_threaded_irq, IRQF_TRIGGER_LOW | IRQF_ONESHOT,
                                    "t1s_hat_fxl6408_irq", dev);
    if (ret < 0) {
        pr_warn("t1s_hat_fxl6408: failed to allocate IRQ\n");
        return ret;
    }
    pr_info("t1s_hat_fxl6408: irq=%d\n", irq);
    return 0;
}

static struct i2c_board_info fxl6408_info = {I2C_BOARD_INFO("fxl6408", FXL6408_I2C_ADDR)};

int t1s_hat_fxl6408_init(struct device* dev) {
    struct i2c_adapter* adapter;
    int ret;

    adapter = i2c_get_adapter(I2C_BUS_ADDRESS);
    if (adapter == NULL) {
        pr_warn("t1s_hat_fxl6408: failed to get i2c adapter\n");
        return -1;
    }
    fxl6408_client = i2c_new_client_device(adapter, &fxl6408_info);
    i2c_put_adapter(adapter);
    if (fxl6408_client == NULL) {
        pr_warn("t1s_hat_fxl6408: failed to create i2c device\n");
        return -1;
    }
    pr_info("t1s_hat_fxl6408: fxl6408 initialized\n");
    init_registers();
    ret = init_interrupt(dev);
    nodeid_thread = kthread_run(get_nodeid, NULL, "t1s_hat_fxl6408_thread");
    wake_up_process(nodeid_thread);
    return ret;
}

void t1s_hat_fxl6408_exit(void) {
    i2c_unregister_device(fxl6408_client);
    kthread_stop(nodeid_thread);
    up(&nodeid_irq_sem);
}
