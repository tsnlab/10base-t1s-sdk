#ifndef _LAN865X_NODEIE_H_
#define _LAN865X_NODEIE_H_

#include <linux/device.h>

#define FXL6408_REG_DEVICE_ID_CTRL    0x1
#define FXL6408_REG_IO_DIRECTION      0x3
#define FXL6408_REG_OUTPUT_STATE      0x5
#define FXL6408_REG_OUTPUT_HIGH_Z     0x7
#define FXL6408_REG_INPUT_DEFAULT     0x9
#define FXL6408_REG_PULL_ENABLE       0xB
#define FXL6408_REG_PULL_UP_PULL_DOWN 0xD
#define FXL6408_REG_INPUT_STATUS      0xF
#define FXL6408_REG_INTERRUPT_MASK    0x11
#define FXL6408_REG_INTERRUPT_STATUS  0x13

int lan865x_nodeid_init(struct device *dev);
void lan865x_nodeid_exit(void);
u8 lan865x_nodeid_read_reg(u8 reg);
void lan865x_nodeid_write_reg(u8 reg, u8 buf);

#endif
