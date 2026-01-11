#ifndef _LAN865X_INTERRUPT_H_
#define _LAN865X_INTERRUPT_H_

int lan865x_interrupt_init(struct spi_device *device);
void lan865x_interrupt_exit(void);

#endif
