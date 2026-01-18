#!/bin/bash

# A simple shell script to check the status of SPI on a Raspberry Pi 4.
# This script verifies the existence of device files

printf "%-8s %-12s %-6s %-12s %-6s  %s\n" DEV DRIVER MODE MAX_HZ IRQ DT_NODE
for d in /sys/bus/spi/devices/spi*; do
  DEV=$(basename "$d")
  DRIVER=$(basename "$(readlink -f "$d/driver" 2>/dev/null)" 2>/dev/null)
  [ -z "$DRIVER" ] && DRIVER="(unbound)"
  MODE=$(cat "$d/mode" 2>/dev/null)
  MAXHZ=$(cat "$d/max_speed_hz" 2>/dev/null)
  IRQ=$(cat "$d/irq" 2>/dev/null)
  [ -z "$IRQ" ] && IRQ="-"   # many SPI slaves (like spidev) have no dedicated IRQ
  DTNODE=$(readlink -f "$d/of_node" 2>/dev/null)
  printf "%-8s %-12s %-6s %-12s %-6s  %s\n" "$DEV" "$DRIVER" "$MODE" "$MAXHZ" "$IRQ" "$DTNODE"
done
