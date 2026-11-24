// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for lares-share.exe, AirDrop without WSL (issue #109), on
 * plain data: the Realtek driver's descriptors, AirDrop's formats, and the
 * IP layer between two links in one process. */
#include "share_check.h"

int share_failures = 0;

int main()
{
    /* Each part says when it starts: a crash then names its part. */
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::fprintf(stderr, "rtl\n");
    run_rtl_tests();
    std::fprintf(stderr, "formats\n");
    run_format_tests();
    std::fprintf(stderr, "net\n");
    run_net_tests();
    if (share_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", share_failures);
        return 1;
    }
    std::printf("lares-share-unit: all checks passed\n");
    return 0;
}
