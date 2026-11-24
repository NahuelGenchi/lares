// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* lwIP's compiler and platform bits for MSVC on Windows (issue #109). */
#include <stdio.h>
#include <stdlib.h>

#define LWIP_NO_UNISTD_H 1
#define BYTE_ORDER LITTLE_ENDIAN

void lares_lwip_assert(const char *msg, int line, const char *file);
#define LWIP_PLATFORM_DIAG(x) do { (void)0; } while (0)
#define LWIP_PLATFORM_ASSERT(x) lares_lwip_assert((x), __LINE__, __FILE__)

#define PACK_STRUCT_BEGIN __pragma(pack(push, 1))
#define PACK_STRUCT_END __pragma(pack(pop))
#define PACK_STRUCT_STRUCT
#define PACK_STRUCT_FIELD(x) x
