// SPDX-License-Identifier: GPL-3.0+
/*
 * T1S HAT FXL6408 GPIO expander driver header
 *
 * Original Author: Harim Seong <harim@tsnlab.com>
 */

#ifndef _T1S_HAT_FXL6408_H_
#define _T1S_HAT_FXL6408_H_

#include <linux/device.h>

struct lan865x_priv;

#define FXL6408_REG_DEVICE_ID_CTRL 0x1
#define FXL6408_REG_IO_DIRECTION 0x3
#define FXL6408_REG_OUTPUT_STATE 0x5
#define FXL6408_REG_OUTPUT_HIGH_Z 0x7
#define FXL6408_REG_INPUT_DEFAULT 0x9
#define FXL6408_REG_PULL_ENABLE 0xB
#define FXL6408_REG_PULL_UP_PULL_DOWN 0xD
#define FXL6408_REG_INPUT_STATUS 0xF
#define FXL6408_REG_INTERRUPT_MASK 0x11
#define FXL6408_REG_INTERRUPT_STATUS 0x13

#define FXL6408_BUTTON_INPUT_MASK (1 << 3)

#define FXL6408_PLCA_OK_LED_ON (1 << 1)
#define FXL6408_PLCA_OK_LED_OFF (0 << 1)

#define FXL6408_BUZZER_ON (1 << 0)
#define FXL6408_BUZZER_OFF (0 << 0)

int t1s_hat_fxl6408_init(struct lan865x_priv* priv, struct device* dev);
void t1s_hat_fxl6408_exit(struct lan865x_priv* priv);
u8 t1s_hat_fxl6408_read_reg(struct lan865x_priv* priv, u8 reg);
void t1s_hat_fxl6408_write_reg(struct lan865x_priv* priv, u8 reg, u8 buf);

#endif
