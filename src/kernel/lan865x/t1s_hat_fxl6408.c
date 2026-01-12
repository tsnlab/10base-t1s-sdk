#include <linux/module.h>
#include <linux/i2c.h>

#include "t1s_hat_fxl6408.h"

#define I2C_BUS_ADDRESS   1
#define FXL6408_I2C_ADDR  0x43
#define FXL6408_I2C_BUS   20
#define FXL6408_MF        0x5

struct fxl6408_device_id_ctrl {
    u8  MF: 3;      // Manufacturer
    u8  FW_rev: 3;  // Firmware version
    u8  RST_INT: 1;
    u8  SW_RST: 1;
};

static struct i2c_client  *fxl6408_client;

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
void init_registers(void)
{
    struct fxl6408_device_id_ctrl   ctrl;
    u8 ret;

    ret = t1s_hat_fxl6408_read_reg(FXL6408_REG_DEVICE_ID_CTRL);
    *(u8 *)&ctrl = ret;
    if (ctrl.MF != FXL6408_MF) {
        pr_warn("t1s_hat_fxl6408: Manufacturer=0x%02x\n", ctrl.MF);
        return;
    }
    ctrl.SW_RST = 1;
    t1s_hat_fxl6408_write_reg(FXL6408_REG_DEVICE_ID_CTRL,
        ret | *(u8 *)&ctrl);
    t1s_hat_fxl6408_write_reg(FXL6408_REG_OUTPUT_HIGH_Z, 0x00);
    t1s_hat_fxl6408_write_reg(FXL6408_REG_IO_DIRECTION, 0x0F);
    t1s_hat_fxl6408_write_reg(FXL6408_REG_INPUT_DEFAULT, 0xF0);
    t1s_hat_fxl6408_write_reg(FXL6408_REG_INTERRUPT_MASK, 0x00);
    ret = t1s_hat_fxl6408_read_reg(FXL6408_REG_DEVICE_ID_CTRL);
}

static struct i2c_board_info  fxl6408_info = {
    I2C_BOARD_INFO("fxl6408", FXL6408_I2C_ADDR)
};

int t1s_hat_fxl6408_init(void)
{
    struct i2c_adapter *adapter;

    adapter = i2c_get_adapter(I2C_BUS_ADDRESS);
    if (adapter == NULL) {
        pr_warn("t1s_hat_fxl6408: failed to get i2c adapter\n");
        return -1;
    }
    fxl6408_client = i2c_new_client_device(adapter, &fxl6408_info);
    init_registers();
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
