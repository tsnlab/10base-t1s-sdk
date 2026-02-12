#!/bin/bash

dtc -@ -I dts -O dtb -o lan8650-rpi4.dtbo lan8650-overlay-rpi4.dts
dtc -@ -I dts -O dtb -o lan8650-rpi5.dtbo lan8650-overlay-rpi5.dts