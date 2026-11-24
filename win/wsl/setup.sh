#!/bin/bash
# SPDX-FileCopyrightText: 2026 Nahuel Genchi
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Sets WSL up for Lares' AirDrop on Windows (issue #98): the packages (BlueZ
# too, for a Bluetooth radio lent to WSL, issue #107), the
# adapter's firmware where the loader can read it, and Lares' own programs
# built into /opt/lares-wsl/bin. Run as root, from the kit's folder:
#   wsl -u root -- bash setup.sh
# It's safe to run again; it only does what's missing. Prints "setup ok" or
# "setup failed <why>".
set -u
KIT=$(dirname "$(readlink -f "$0")")
OUT=/opt/lares-wsl/bin
[ "$(id -u)" = "0" ] || { echo "setup failed not-root"; exit 1; }

need=""
for p in gcc pkg-config iw lsusb zstd bluetoothctl; do command -v $p >/dev/null || need=1; done
pkg-config --exists gio-unix-2.0 2>/dev/null || need=1
[ -e /usr/lib/x86_64-linux-gnu/gio/modules/libgiognutls.so ] || need=1
[ -e /lib/firmware/rtlwifi/rtl8188eufw.bin.zst ] || [ -e /lib/firmware/rtlwifi/rtl8188eufw.bin ] || need=1
if [ -n "$need" ]; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/dev/null 2>&1
  apt-get install -y -qq --no-install-recommends gcc libc6-dev pkg-config libglib2.0-dev glib-networking iw usbutils \
    kmod zstd wireless-regdb linux-firmware-realtek bluez >/dev/null 2>&1 || { echo "setup failed packages"; exit 1; }
fi

# The WSL kernel reads no zstd firmware.
fw=/lib/firmware/rtlwifi/rtl8188eufw.bin
[ -e $fw ] || zstd -dkfq $fw.zst || { echo "setup failed firmware"; exit 1; }

# The driver is loaded by bringup.sh only, which feeds it its firmware: one
# that loaded by itself on attach would wait for firmware nobody brings.
echo "blacklist rtl8xxxu" > /etc/modprobe.d/lares-wsl.conf

mkdir -p $OUT
CFLAGS="-std=gnu11 -O2 -Wall"
gcc $CFLAGS -o $OUT/rtlwake "$KIT/rtlwake.c" || { echo "setup failed build-rtlwake"; exit 1; }
gcc $CFLAGS -o $OUT/lares-awdl "$KIT"/src/awdl/*.c || { echo "setup failed build-awdl"; exit 1; }
gcc $CFLAGS $(pkg-config --cflags gio-2.0 gio-unix-2.0) -o $OUT/lares-share "$KIT/src/sharecli/main.c" "$KIT/src/share.c" \
  "$KIT"/src/airdrop/*.c $(pkg-config --libs gio-2.0 gio-unix-2.0) -lm || { echo "setup failed build-share"; exit 1; }
install -m755 "$KIT/bringup.sh" "$KIT/lares-awdl-wsl" "$KIT/lares-bt-wsl" "$KIT/run.sh" $OUT/
sed -i 's/\r$//' $OUT/bringup.sh $OUT/lares-awdl-wsl $OUT/lares-bt-wsl $OUT/run.sh
cat "$KIT/VERSION" > $OUT/VERSION 2>/dev/null
echo "setup ok"
