// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* Bounded FileIcon preview thumbnails for outgoing AirDrop photo and video
 * shares (issue #117). Uses Windows Imaging Component (WIC) for images and
 * Windows Shell's IShellItemImageFactory for videos and shell-decoded
 * formats, keeping output small so /Ask stays fast on 2.4 GHz channel 6. */
#include <cstddef>
#include <string>

#include "formats.h"

namespace lares::airdrop {

constexpr size_t kFileIconMaxBytes = 64 * 1024;
constexpr unsigned kFileIconMaxDim = 320;

/* Whether `path`'s extension is a photo or video format that should carry a
 * preview thumbnail in /Ask. */
bool file_icon_supported(const std::wstring& path);

/* Produces a small bounded JPEG (or PNG fallback) thumbnail for `path`.
 * Returns false without modifying `out` when unsupported, unreadable, or
 * larger than `max_bytes`. Never throws. */
bool make_file_icon(const std::wstring& path, Bytes& out, size_t max_bytes = kFileIconMaxBytes,
                    unsigned max_dim = kFileIconMaxDim);

} // namespace lares::airdrop
