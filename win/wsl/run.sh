#!/bin/bash
# SPDX-FileCopyrightText: 2026 Nahuel Genchi
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runs lares-share for the Windows window (issue #98):
#   run.sh <Windows Downloads folder, as a WSL path>
# Files received go to that folder, and the link is lares-awdl-wsl, which
# borrows the adapter from Windows.
BIN=$(dirname "$(readlink -f "$0")")
export HOME=/opt/lares-wsl/home
mkdir -p "$HOME/.config" "$HOME/.cache"
printf 'XDG_DOWNLOAD_DIR="%s"\n' "${1:-$HOME/Downloads}" > "$HOME/.config/user-dirs.dirs"
export XDG_RUNTIME_DIR=/run/lares-wsl
mkdir -p $XDG_RUNTIME_DIR
export LARES_AWDL="$BIN/lares-awdl-wsl"
export LARES_WIFI=wlan0
exec "$BIN/lares-share"
