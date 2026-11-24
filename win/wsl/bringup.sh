#!/bin/bash
# SPDX-FileCopyrightText: 2026 Nahuel Genchi
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Brings an RTL8188EU that Windows just handed to WSL up as a Wi-Fi
# interface, without starting it: lares-awdl starts it, once (issue #98).
#
# - The Windows driver leaves the chip's MCU IO wrapper held in reset, and
#   rtl8xxxu's firmware then never starts: rtlwake releases it first.
# - The WSL kernel looks for firmware in its own first root, not in the
#   distribution's /lib/firmware, so the firmware goes through the kernel's
#   sysfs loader; udev would answer that request with "not found", so it
#   waits meanwhile.
# - The driver doesn't always take at the first try after an attach.
# - rtl8xxxu retries unicast 48 times whatever radiotap asks, which holds up
#   the link: rtlwake sets the retry limit to 0 once it has the chip (#107).
#
# Prints "ready <ifname>" or "failed <why>".
BIN=$(dirname "$(readlink -f "$0")")
FW=/lib/firmware/rtlwifi/rtl8188eufw.bin
ID=0bda:8179
log() { echo "bringup: $(cut -d" " -f1 /proc/uptime) $*" >&2; }

for i in $(seq 1 40); do lsusb | grep -q $ID && break; sleep 0.25; done
lsusb | grep -q $ID || { echo "failed no-adapter"; exit 1; }
[ -r "$FW" ] || { echo "failed no-firmware"; exit 1; }
sleep 2   # let it finish enumerating

echo 1 > /proc/sys/kernel/firmware_config/force_sysfs_fallback
echo 8 > /sys/class/firmware/timeout 2>/dev/null   # a request nobody answers fails soon
systemctl stop systemd-udevd-kernel.socket systemd-udevd-control.socket systemd-udevd 2>/dev/null
trap 'systemctl start systemd-udevd 2>/dev/null' EXIT

for try in 1 2 3 4; do
  modprobe -r rtl8xxxu 2>/dev/null
  dev=$(lsusb | grep $ID | sed -E 's/Bus ([0-9]+) Device ([0-9]+).*/\/dev\/bus\/usb\/\1\/\2/')
  log "try $try on $dev"
  "$BIN/rtlwake" "$dev" >&2 || { sleep 1; continue; }
  ( modprobe rtl8xxxu ) &
  fed=""
  for i in $(seq 1 400); do
    for l in /sys/class/firmware/*/loading; do
      [ -e "$l" ] || continue
      d=${l%/loading}
      echo 1 > "$l"; cat "$FW" > "$d/data"; echo 0 > "$l"
      fed=1
    done
    [ -n "$fed" ] && break
    sleep 0.02
  done
  [ -n "$fed" ] || log "try $try: the driver never asked for its firmware"
  wait
  ifname=""
  for i in $(seq 1 24); do
    ifname=$(ls /sys/class/ieee80211/*/device/net 2>/dev/null | head -1)
    [ -n "$ifname" ] && break
    sleep 0.25
  done
  if [ -n "$ifname" ]; then
    log "try $try: $ifname"
    "$BIN/rtlwake" "$dev" noretry >&2 || log "the retry limit stays as rtl8xxxu set it"
    echo "ready $ifname"
    exit 0
  fi
  log "try $try: the driver didn't take"
  sleep 1
done
echo "failed driver"
exit 1
