# README – 10Base-t1s Raspberry Linux Module Driver

This document describes how to build and deploy the Microchip LAN865x 10BASE-T1S MAC-PHY driver on Raspberry Pi (RPi4 / RPi5).

# 1. Prerequisites
Linux packages (on host PC)

Make sure the following packages are installed:

```bash
$ sudo apt update
$ sudo apt install -y build-essential git bc bison flex libssl-dev \
    libncurses5-dev libncursesw5-dev dwarves device-tree-compiler
```

Kernel source or headers

You need the kernel source tree or headers that match your target Raspberry Pi kernel.

```bash
$ git clone -b rpi-6.6.y --depth=1 https://github.com/raspberrypi/linux ~/rpi-linux
$ cd ~/rpi-linux
$ make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- bcm2711_defconfig
$ make -j$(nproc) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image modules dtbs
$ make -j$(nproc) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- INSTALL_MOD_PATH=~/modules modules_install
# You need to transfer the compiled kernel to the Raspberry Pi board and boot it.
# Ref: https://www.raspberrypi.com/documentation/computers/linux_kernel.html
```

# 2. Environment Variables

Define kernel source directory depending on your board.

For RPi4
```bash
export RPI4_KERNEL_DIR=/home/you/workspace/rpi4/rpi-linux
```

For RPi5
```bash
export RPI5_KERNEL_DIR=/home/you/workspace/rpi5/rpi-linux
```

These are referenced in the Makefile (KERNEL_DIR).

# 3. Build the Driver
Options

BOARD=rpi4 or BOARD=rpi5 (default is rpi4)

MACADDR=xx:xx:xx:xx:xx:xx → Use fixed MAC

MACADDR=none → Random MAC (runtime)

DISABLE_DIPSWITCH=1 → Disable DIP switch extension

Examples
```bash
# Build for Raspberry Pi 4, default MAC, DIP switch enabled
$ make BOARD=rpi4
```
or BOARD=rpi4 is omittable
```bash
# Build for Raspberry Pi 4, default MAC, DIP switch enabled
$ make
```

```bash
# Build for Raspberry Pi 4 with fixed MAC
$ make BOARD=rpi4 MACADDR="de:ad:be:ef:12:34"
```
```bash
# Build with random MAC, DIP switch enabled
$ make BOARD=rpi4 MACADDR=none
```
```bash
# Build with default MAC, DIP switch disabled
$ make BOARD=rpi4 DISABLE_DIPSWITCH=1
```

The result is:

lan865x.ko

# 4. Device Tree Overlay (DTS)

A sample overlay source is provided: lan8650-overlay.dts

Compile overlay device tree
```bash
$ chmod +x ./tools/dts-scripts.sh
$ ./tools/dts-scripts.sh
```

Deploy overlay

Copy the compiled file into overlay directory:

```bash
$ sudo cp lan8650-[rpi4|rpi5].dtbo /boot/firmware/overlays/
```

Update config.txt

```bash
$ sudo nano /boot/firmware/config.txt
```

Enable spi and i2c
```bash
# Uncomment some or all of these to enable the optional hardware interfaces
dtparam=i2c_arm=on
#dtparam=i2s=on
dtparam=spi=on
```

Set the overlay device tree
```bash
[all]
dtoverlay=lan8650-[rpi4|rpi5]
```

Reboot:
```bash
$ sudo reboot
```

<!--
Check device environment

spi device info
```bash
$ source tools/show-spi.sh

DEV      DRIVER       MODE   MAX_HZ       IRQ     DT_NODE
spi0.0   driver                           -       /sys/firmware/devicetree/base/soc/spi@7e204000/ethernet@0
spi0.1   spidev                           -       /sys/firmware/devicetree/base/soc/spi@7e204000/spidev@1
```

i2c device info
```bash
$ sudo apt-get update
$ sudo apt-get install i2c-tools
$ i2cdetect -l
i2c-1   unknown         bcm2835 (i2c@7e804000)                  N/A
i2c-20  unknown         fef04500.i2c                            N/A
i2c-21  unknown         fef09500.i2c                            N/A
$ sudo i2cget -y 1 0x43 0x0f
<pin node_id>
```
-->

# 5. Clean

```bash
make clean
```

# 6. Load/Unload the Driver

```bash
$ sudo insmod lan865x.ko
```
Check the driver's probing logs

```bash
$ dmesg | tail -n 20

### dmesg Log (LAN8650 Probe)
| Timestamp      | Message                                                                 |
|----------------|-------------------------------------------------------------------------|
| 1636.011154    | lan8650 spi0.0: >>> probe start (bus=0, cs=0, max_hz=25000000, mode=7)  |
| 1636.011207    | lan865x: requesting IRQ 56                                              |
| 1636.011222    | oa_tc6_sw_reset_macphy: starting soft reset                             |
| 1636.011251    | oa_tc6_sw_reset_macphy: wrote RESET=0x00000001                          |
| 1636.011269    | oa_tc6_sw_reset_macphy: reset complete, STATUS0=0x00000040              |
| 1636.011287    | oa_tc6_sw_reset_macphy: cleared STATUS0=0x00000040                      |
| 1636.214509    | Generic PHY spi0.0:00: attached PHY driver (mii_bus:phy_addr=spi0.0:00, irq=POLL) |
| 1636.215653    | lan8650 spi0.0: Using build-time MAC d0:d1:95:30:23:00                  |
| 1636.215793    | lan8650 spi0.0: DIPSW: NodeID=14 (from raw=0xe4)                        |
| 1636.215800    | lan865x: set_nodeid=14 (regval=0x0000080e)                              |
| 1636.215811    | lan8650 spi0.0: MAC updated with NodeID=14 -> d0:d1:95:30:23:0e         |
| 1636.216179    | lan8650 spi0.0: LAN865x registered with MAC d0:d1:95:30:23:0e           |
```

Check the loaded driver
```bash
$ sudo lsmod | grep lan865x
```

Remove the loaded driver
```bash
$ sudo rmmod lan865x
```

Check the driver's unloading logs
```bash
$ dmesg | tail -n 20
### dmesg Log (LAN8650 Remove)
| Timestamp    | Message                                                                 |
|--------------|-------------------------------------------------------------------------|
| 1777.964069  | lan8650 spi0.0: lan865x: remove start                                   |
| 1777.964100  | lan8650 spi0.0: lan865x: unregistering netdev eth1                      |
| 1777.964110  | lan8650 spi0.0: lan865x: MAC before unregister d0:d1:95:30:23:0e        |
| 1777.988594  | lan8650 spi0.0: lan865x: MAC after unregister d0:d1:95:30:23:0e         |
| 1777.988624  | lan8650 spi0.0: lan865x: calling oa_tc6_exit                            |
| 1777.991729  | lan8650 spi0.0: lan865x: calling free_netdev                            |
| 1777.991746  | lan8650 spi0.0: lan865x: MAC before free d0:d1:95:30:23:0e              |
| 1777.991794  | lan8650 spi0.0: lan865x: remove finished                                |
```
