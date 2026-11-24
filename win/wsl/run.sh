#!/bin/bash
# SPDX-FileCopyrightText: 2026 Nahuel Genchi
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runs lares-share for the Windows window (issue #98):
#   run.sh <Windows Downloads folder, as a WSL path> [<Bluetooth radio's usbipd ID>]
# Files received go to that folder, and the link is lares-awdl-wsl, which
# borrows the adapter from Windows. With a Bluetooth radio named, one that
# can't advertise from Windows, lares-bt-wsl lends it to WSL while the
# wake-up is wanted (issue #107).
BIN=$(dirname "$(readlink -f "$0")")
export HOME=/opt/lares-wsl/home
mkdir -p "$HOME/.config" "$HOME/.cache"
printf 'XDG_DOWNLOAD_DIR="%s"\n' "${1:-$HOME/Downloads}" > "$HOME/.config/user-dirs.dirs"
export XDG_RUNTIME_DIR=/run/lares-wsl
mkdir -p $XDG_RUNTIME_DIR
export LARES_AWDL="$BIN/lares-awdl-wsl"
export LARES_WIFI=wlan0
if [ -n "$2" ]; then
  export LARES_BT="$BIN/lares-bt-wsl" LARES_BT_ID="$2"
fi
exec "$BIN/lares-share"
