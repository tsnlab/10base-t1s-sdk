// SPDX-License-Identifier: GPL-2.0+
/*
 * LAN865x node ID interrupt implementation
 */

#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>

#include "lan865x_interrupt.h"

static irqreturn_t irq_handler(int irq, void* dev)
{
  pr_info("lan865x: irq_handler called\n");
  return IRQ_HANDLED;
}

int lan865x_interrupt_init(struct spi_device *spi_device)
{
    struct gpio_desc *gpiod;
    struct device *device = &spi_device->dev;
    int irq;
    int ret;

    gpiod = devm_gpiod_get(device, "nodeid", GPIOD_IN);
    if (IS_ERR(gpiod)) {
      return PTR_ERR(gpiod);
    }
    irq = gpiod_to_irq(gpiod);
    if (irq < 0) {
      pr_err("lan865x: failed to get IRQ number\n");
      return irq;
    }
    ret = devm_request_threaded_irq(device, irq, NULL, irq_handler,
                             IRQF_TRIGGER_LOW | IRQF_ONESHOT,
                             "lan865x_irq",
                             device);
    if (ret < 0) {
      pr_err("lan865x: failed to allocate IRQ\n");
      return ret;
    }
    pr_info("lan865x: irq=%d\n", irq);
    return 0;
}

void lan865x_interrupt_exit(void)
{

}
