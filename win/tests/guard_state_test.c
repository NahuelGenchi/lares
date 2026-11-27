/* SPDX-FileCopyrightText: 2026 Nahuel Genchi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The guard's state file as plain data: its text form, a file cut short, and
 * the five-minute crash count.
 */
#include <stdio.h>
#include <string.h>

#include "safe.h"
#include "state.h"

static int failures;

static void expect(int ok, const char *what)
{
    if (ok) return;
    ++failures;
    printf("  FAILED [state]: %s\n", what);
}

int guard_state_tests(void)
{
    lares_guard_state a, b;
    char text[1024];
    int n;

    memset(&a, 0, sizeof a);
    a.running = 1;
    a.hidden = 1;
    a.autohide_before = -1;
    a.guard_pid = 1234;
    a.twin_pid = 5678;
    a.shell_pid = 91011;
    a.crash_count = 2;
    a.crashes[0] = 134000000000000000ull;
    a.crashes[1] = 134000000012345678ull;
    n = lares_guard_state_format(&a, text, sizeof text);
    lares_guard_state_parse(text, n, &b);
    expect(memcmp(&a, &b, sizeof a) == 0, "a state reads back as it was written");

    /* Every shorter file is one that was cut while being written. */
    for (int cut = 0; cut < n; ++cut) {
        lares_guard_state_parse(text, cut, &b);
        if (b.running || b.hidden || b.crash_count) {
            expect(0, "a file cut short is believed");
            break;
        }
    }
    lares_guard_state_parse("something else\nhidden 1\nend\n", 28, &b);
    expect(!b.hidden, "a file that is not a state file is ignored");

    ULONGLONG minute = 60ull * 10000000ull, t = 134000000000000000ull;
    memset(&a, 0, sizeof a);
    expect(lares_guard_state_add_crash(&a, t) == 1, "first crash");
    expect(lares_guard_state_add_crash(&a, t + 4 * minute) == 2, "second crash four minutes later counts");
    expect(lares_guard_state_add_crash(&a, t + 6 * minute) == 2, "the first is forgotten after five minutes");
    expect(lares_guard_state_add_crash(&a, t + 20 * minute) == 1, "a crash long after stands alone");
    for (int i = 0; i < 10; ++i) n = lares_guard_state_add_crash(&a, t + 20 * minute + (ULONGLONG)i);
    expect(n == LARES_GUARD_MAX_CRASHES && a.crash_count == LARES_GUARD_MAX_CRASHES, "the list stays bounded");
    expect(LARES_GUARD_CRASH_LIMIT == 2, "two crashes stop the restarts");

    /* Safe mode's decision: the switch, the session's mark, or Shift, and
     * Shift only for a start at sign-in. */
    expect(!lares_safe_mode_wanted(0, 0, 0, 0), "nothing asked: no safe mode");
    expect(lares_safe_mode_wanted(1, 0, 0, 0), "the --safe switch");
    expect(lares_safe_mode_wanted(0, 0, 0, 1), "a marked session");
    expect(lares_safe_mode_wanted(0, 1, 1, 0), "Shift held at sign-in");
    expect(!lares_safe_mode_wanted(0, 0, 1, 0), "Shift held at any other start does not count");
    expect(!lares_safe_mode_wanted(0, 1, 0, 0), "a sign-in without Shift is a normal start");

    printf("state file and safe mode's decision: %s\n", failures ? "FAILED" : "ok");
    return failures;
}
