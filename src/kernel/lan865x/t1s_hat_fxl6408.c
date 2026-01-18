#include "t1s_hat_fxl6408.h"
#include "lan865x_arch.h"

#include <linux/atomic/atomic-instrumented.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/semaphore.h>

#define I2C_BUS_ADDRESS 1
#define FXL6408_I2C_ADDR 0x43
#define FXL6408_I2C_BUS 20
#define FXL6408_MF 0x05
#define FXL6408_REG_SIZE 1
static struct task_struct* nodeid_thread;
static DEFINE_SEMAPHORE(nodeid_irq_sem, 0);

static atomic_t nodeid_irq_flag = ATOMIC_INIT(0);

u8 t1s_hat_fxl6408_read_reg(struct lan865x_priv* priv, u8 reg) {
    struct i2c_msg msgs[2];
    u8 val = 0;
    int ret;

    if (!priv || !priv->fxl6408_client) {
        pr_err("t1s_hat_fxl6408: priv or fxl6408_client is NULL\n");
        return -EINVAL;
    }

    msgs[0].addr = FXL6408_I2C_ADDR;
    msgs[0].flags = 0;
    msgs[0].len = FXL6408_REG_SIZE;
    msgs[0].buf = &reg;

    msgs[1].addr = FXL6408_I2C_ADDR;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len = FXL6408_REG_SIZE;
    msgs[1].buf = &val;

    mutex_lock(&priv->fxl6408_lock);
    ret = i2c_transfer(priv->fxl6408_client->adapter, msgs, sizeof(msgs) / sizeof(struct i2c_msg));
    if (ret < 0) {
        pr_err("t1s_hat_fxl6408: i2c_transfer failed (ret=%d)\n", ret);
        mutex_unlock(&priv->fxl6408_lock);
        return ret;
    }
    mutex_unlock(&priv->fxl6408_lock);

    return val;
}

void t1s_hat_fxl6408_write_reg(struct lan865x_priv* priv, u8 reg, u8 val) {
    struct i2c_msg msg;
    u8 data[2] = { reg, val };
    int ret;

    if (!priv || !priv->fxl6408_client) {
        pr_err("t1s_hat_fxl6408: priv or fxl6408_client is NULL\n");
        return;
    }

    msg.addr = FXL6408_I2C_ADDR;
    msg.flags = 0;
    msg.len = 2;
    msg.buf = data;

    mutex_lock(&priv->fxl6408_lock);
    ret = i2c_transfer(priv->fxl6408_client->adapter, &msg, 1);
    if (ret < 0) {
        pr_err("t1s_hat_fxl6408: i2c_transfer failed (ret=%d)\n", ret);
    }
    mutex_unlock(&priv->fxl6408_lock);
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
    struct lan865x_priv* priv = (struct lan865x_priv*)data;
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

        /* Wait for 20ms to avoid interrupt storm. */
        msleep(20);

        /* Read input status register */
        val = t1s_hat_fxl6408_read_reg(priv, FXL6408_REG_INPUT_STATUS);

        /* If button is pressed, set node_id to LAN8650 */
        if (val & FXL6408_BUTTON_INPUT_MASK) {
            priv->node_id = (val & 0xF0) >> 4;
            pr_debug ("t1s_hat_fxl6408: node_id = 0x%02x\n", priv->node_id);
            lan865x_set_nodeid(priv, priv->node_id);
        }

        atomic_set(&nodeid_irq_flag, 0);
    }
    return 0;
}

static irqreturn_t nodeid_threaded_irq(int irq, void* data) {
    struct lan865x_priv* priv = (struct lan865x_priv*)data;

    if (atomic_cmpxchg(&nodeid_irq_flag, 0, 1) == 0) {
        up(&nodeid_irq_sem);
    }
    if (!priv) {
        pr_err("t1s_hat_fxl6408: priv is NULL\n");
        return IRQ_NONE;
    }

    t1s_hat_fxl6408_read_reg(priv, FXL6408_REG_INTERRUPT_STATUS);

    return IRQ_HANDLED;
}

static void init_registers(struct lan865x_priv* priv) {
    struct fxl6408_device_id_ctrl {
        u8 SW_RST : 1;
        u8 RST_INT : 1;
        u8 FW_rev : 3; // Firmware version
        u8 MF : 3;     // Manufacturer
    } ctrl;
    u8 ret;

    ret = t1s_hat_fxl6408_read_reg(priv, FXL6408_REG_DEVICE_ID_CTRL);
    *(u8*)&ctrl = ret;
    if (ctrl.MF != FXL6408_MF) {
        pr_warn("t1s_hat_fxl6408: Manufacturer=0x%02x\n", ctrl.MF);
    }
    ctrl.SW_RST = 1;
    t1s_hat_fxl6408_write_reg(priv, FXL6408_REG_INPUT_DEFAULT, 0x00);
    t1s_hat_fxl6408_write_reg(priv, FXL6408_REG_DEVICE_ID_CTRL, *(u8*)&ctrl);
    t1s_hat_fxl6408_write_reg(priv, FXL6408_REG_IO_DIRECTION, 0x03);
    t1s_hat_fxl6408_write_reg(priv, FXL6408_REG_OUTPUT_HIGH_Z, 0x00);
    t1s_hat_fxl6408_write_reg(priv, FXL6408_REG_INTERRUPT_MASK, 0x00);
    ret = t1s_hat_fxl6408_read_reg(priv, FXL6408_REG_DEVICE_ID_CTRL);
}

static int init_interrupt(struct lan865x_priv* priv, struct device* dev) {
    struct gpio_desc* gpiod;
    int irq;
    int ret;

    /* Check if device tree node exists */
    if (!dev->of_node) {
        pr_err("t1s_hat_fxl6408: device does not have device tree node\n");
        return -ENODEV;
    }

    pr_info("t1s_hat_fxl6408: device tree node exists: %pOF\n", dev->of_node);

    /* nodeid-gpios */
    gpiod = devm_gpiod_get(dev, "nodeid", GPIOD_IN);
    if (IS_ERR(gpiod)) {
        ret = PTR_ERR(gpiod);
        pr_err("t1s_hat_fxl6408: failed to get nodeid GPIO (ret=%d, ENOENT=%d)\n", 
               ret, -ENOENT);
        if (ret == -ENOENT) {
            pr_err("t1s_hat_fxl6408: nodeid-gpios property not found in device tree\n");
            pr_err("t1s_hat_fxl6408: device tree node: %pOF\n", dev->of_node);
        }
        return ret;
    }

    irq = gpiod_to_irq(gpiod);
    if (irq < 0) {
        pr_warn("t1s_hat_fxl6408: failed to get IRQ number\n");
        return irq;
    }

    ret = devm_request_threaded_irq(dev, irq, NULL, nodeid_threaded_irq, IRQF_TRIGGER_LOW | IRQF_ONESHOT,
                                    "t1s_hat_fxl6408_irq", priv);

    if (ret < 0) {
        pr_warn("t1s_hat_fxl6408: failed to allocate IRQ\n");
        return ret;
    }

    pr_info("t1s_hat_fxl6408: irq=%d\n", irq);
    return 0;
}

static struct i2c_board_info fxl6408_info = {I2C_BOARD_INFO("fxl6408", FXL6408_I2C_ADDR)};

int t1s_hat_fxl6408_init(struct lan865x_priv* priv, struct device* dev) {
    struct i2c_adapter* adapter;
    int ret;

    if (!priv) {
        pr_warn("t1s_hat_fxl6408: priv is NULL\n");
        return -EINVAL;
    }

    mutex_init(&priv->fxl6408_lock);

    adapter = i2c_get_adapter(I2C_BUS_ADDRESS);
    if (adapter == NULL) {
        pr_warn("t1s_hat_fxl6408: failed to get i2c adapter\n");
        return -1;
    }

    priv->fxl6408_client = i2c_new_client_device(adapter, &fxl6408_info);
    i2c_put_adapter(adapter);
    if (priv->fxl6408_client == NULL) {
        pr_warn("t1s_hat_fxl6408: failed to create i2c device\n");
        return -1;
    }

    init_registers(priv);
    ret = init_interrupt(priv, dev);
    nodeid_thread = kthread_run(get_nodeid, priv, "t1s_hat_fxl6408_thread");
    wake_up_process(nodeid_thread);

    priv->fxl6408_initialized = true;
    pr_info("t1s_hat_fxl6408: fxl6408 initialized\n");

    return ret;
}

void t1s_hat_fxl6408_exit(struct lan865x_priv* priv) {
    if (!priv) {
        pr_warn("t1s_hat_fxl6408: priv is NULL\n");
        return;
    }

    if (priv->fxl6408_client) {
        i2c_unregister_device(priv->fxl6408_client);
        priv->fxl6408_client = NULL;
    }
    if (nodeid_thread) {
        kthread_stop(nodeid_thread);
        nodeid_thread = NULL;
    }
    up(&nodeid_irq_sem);
}
