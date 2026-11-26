// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* lares-share-unit's checks (issue #109): each test file adds a run_*
 * function, which share_unit.cpp calls. */
#include <cstdio>

extern int share_failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++share_failures; \
    } \
} while (0)

void run_driver_tests();
void run_rtl_tests();
void run_format_tests();
void run_net_tests();
