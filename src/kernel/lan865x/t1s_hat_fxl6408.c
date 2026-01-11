#include <linux/module.h>
#include <linux/i2c.h>

#include "t1s_hat_fxl6408.h"

#define I2C_BUS_ADDRESS   1
#define FXL6408_I2C_ADDR  0x43
#define FXL6408_I2C_BUS   20

static struct i2c_client  *fxl6408_client;

/*
u8 t1s_hat_fxl6408_read_reg(u8 reg)
{
    u8  buf;

    i2c_master_send(fxl6408_client, &reg, 1);
    i2c_master_recv(fxl6408_client, &buf, 1);
    return buf;
}

void t1s_hat_fxl6408_write_reg(u8 reg, u8 val)
{
    i2c_master_send(fxl6408_client, &reg, 1);
    i2c_master_send(fxl6408_client, &val, 1);
}
*/

void t1s_hat_fxl6408_write_reg(u8 reg, u8 val)
{
    struct i2c_adapter *adap;
    struct i2c_msg msgs[2];
    u8 val = 0;
    int ret;

    adap = i2c_get_adapter(1);
    if (!adap) {
        dev_err(dev, "DIPSW: failed to get i2c adapter 1\n");
        return -ENODEV;
    }

    /* write reg address (0x0F) */
    msgs[0].addr  = FXL6408_I2C_ADDR;
    msgs[0].flags = 0;
    msgs[0].len   = 1;
    msgs[0].buf   = &reg;

    /* read data */
    msgs[1].addr  = FXL6408_I2C_ADDR;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len   = 1;
    msgs[1].buf   = &val;

    ret = i2c_transfer(adap, msgs, sizeof(msgs)/sizeof(struct i2c_msg));
    i2c_put_adapter(adap);

    if (ret < 0) {
        pr_err("lan865x: FXL6408 i2c_transfer failed (ret=%d)\n", ret);
        return ret;
    }

    return val;
}

void t1s_hat_fxl6408_write_reg(u8 reg, u8 val)
{
    struct i2c_adapter *adap;
    struct i2c_msg msgs[2];
    u8 val = 0;
    int ret;

    adap = i2c_get_adapter(1);
    if (!adap) {
        dev_err(dev, "DIPSW: failed to get i2c adapter 1\n");
        return;
    }

    /* write reg address (0x0F) */
    msgs[0].addr  = FXL6408_I2C_ADDR;
    msgs[0].flags = 0;
    msgs[0].len   = 1;
    msgs[0].buf   = &reg;

    /* read data */
    msgs[1].addr  = FXL6408_I2C_ADDR;
    msgs[1].flags = 0;
    msgs[1].len   = 1;
    msgs[1].buf   = &val;

    ret = i2c_transfer(adap, msgs, sizeof(msgs)/sizeof(struct i2c_msg));
    i2c_put_adapter(adap);

    if (ret < 0) {
        pr_err("lan865x: FXL6408 i2c_transfer failed (ret=%d)\n", ret);
        return;
    }
}

static struct i2c_board_info  fxl6408_info = {
    I2C_BOARD_INFO("fxl6408", FXL6408_I2C_ADDR)
};

int t1s_hat_fxl6408_init(void)
{
    struct i2c_adapter *adapter;
    int   ret;

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
    return 0;
}

void t1s_hat_fxl6408_exit(void)
{
  i2c_unregister_device(fxl6408_client);
}
