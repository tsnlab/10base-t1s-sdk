#!/usr/bin/env bash
# Reference https://www.raspberrypi.com/documentation/computers/linux_kernel.html
# This script does
# 1. Replace kernel image and modules of Raspberry Pi OS
#    for the driver that requires specific linux kernel version (6.6.y).
# 2. Cross-compile driver module and compile device tree.
# 3. Transfer outputs to SD card.

if [ "${RPI4_KERNEL_DIR}" == "" ]; then
  echo "RPI4_KERNEL_DIR is not defined" > /dev/stderr
  exit 1
fi

# Raspberry Pi 4
KERNEL=kernel8

# BOOT_PARTITION=/dev/sda1
# ROOT_PARTITION=/dev/sda2
BOOT_PARTITION=/dev/mmcblk0p1
ROOT_PARTITION=/dev/mmcblk0p2
echo "NOTE:  ${BOOT_PARTITION}  ${ROOT_PARTITION}"
echo "will be mounted and written. Press enter to proceed."
read -r

BOOT_MOUNTPOINT=${RPI4_KERNEL_DIR}/mnt/boot
ROOT_MOUNTPOINT=${RPI4_KERNEL_DIR}/mnt/root

if [ ! -d "${BOOT_MOUNTPOINT}" ]; then
  mkdir -p "${BOOT_MOUNTPOINT}" "${ROOT_MOUNTPOINT}"
fi

kernel() {
  pushd "${RPI4_KERNEL_DIR}"

  echo "writing kernel modules"
  sudo env PATH="${PATH}" make -j"$(nproc)" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- INSTALL_MOD_PATH="${ROOT_MOUNTPOINT}" modules_install

  echo "writing kernel image"
  sudo cp "${BOOT_MOUNTPOINT}"/"${KERNEL}".img "${BOOT_MOUNTPOINT}"/"${KERNEL}"-backup.img
  sudo cp arch/arm64/boot/Image "${BOOT_MOUNTPOINT}"/"${KERNEL}".img

  echo "writing device tree files"
  sudo cp arch/arm64/boot/dts/broadcom/*.dtb "${BOOT_MOUNTPOINT}"
  sudo cp arch/arm64/boot/dts/overlays/*.dtb* "${BOOT_MOUNTPOINT}"/overlays/
  sudo cp arch/arm64/boot/dts/overlays/README "${BOOT_MOUNTPOINT}"/overlays/

  popd
}

driver() {
  dtc -@ -I dts -O dtb -o lan8650.dtbo lan8650-overlay.dts
  make BOARD=rpi4
  if [[ $(grep "dtoverlay=lan8650" "${BOOT_MOUNTPOINT}"/config.txt) == "" ]]; then
    sudo sed -i 's/\[all\]/\0\ndtoverlay=lan8650/' "${BOOT_MOUNTPOINT}"/config.txt
  fi
  sudo sed -i 's/#dtparam=i2c_arm=on/dtparam=i2c_arm=on/' "${BOOT_MOUNTPOINT}"/config.txt
  sudo sed -i 's/#dtparam=spi=on/dtparam=spi=on/' "${BOOT_MOUNTPOINT}"/config.txt
  if [[ $(grep "i2c" "${ROOT_MOUNTPOINT}"/etc/modules) == "" ]]; then
    sudo echo "i2c-bcm2835" | sudo tee -a "${ROOT_MOUNTPOINT}"/etc/modules
    sudo echo "i2c-brcmstb" | sudo tee -a "${ROOT_MOUNTPOINT}"/etc/modules
  fi
  sudo cp lan8650.dtbo "${BOOT_MOUNTPOINT}"/overlays/
  sudo cp lan865x.ko "${ROOT_MOUNTPOINT}"/opt/
  sudo cp config.txt "${BOOT_MOUNTPOINT}"/overlays/
}

if ! sudo mount "${BOOT_PARTITION}" "${BOOT_MOUNTPOINT}"; then
  echo "mount error"
  exit 1
fi
if ! sudo mount "${ROOT_PARTITION}" "${ROOT_MOUNTPOINT}"; then
  echo "mount error"
  exit 1
fi

set -e

# kernel
driver

sudo umount "${BOOT_MOUNTPOINT}" "${ROOT_MOUNTPOINT}"
sync
echo "safe to remove sd card"
