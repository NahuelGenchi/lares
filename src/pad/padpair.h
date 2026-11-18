// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <gio/gio.h>

/* Pair a Controller: finds Xbox controllers waiting to pair, then pairs,
 * trusts and connects the one picked, through BlueZ. Ported from Padlight's
 * pairing.js. */

typedef enum {
    PAIR_IDLE, PAIR_STARTING, PAIR_UNAVAILABLE, PAIR_OFF, PAIR_SEARCHING,
    PAIR_NOT_FOUND, PAIR_STOPPED, PAIR_PAIRING, PAIR_CONNECTED, PAIR_FAILED
} PairState;

typedef struct {
    char *path, *name;
} PairFound;

typedef struct PadPair PadPair;
typedef void (*PadPairNotify)(gpointer user);

PadPair *padpair_new(PadPairNotify changed, gpointer user);
void padpair_search(PadPair *p);
void padpair_pair(PadPair *p, const char *path);
void padpair_cancel(PadPair *p);
/* A search ends when nobody's looking; a pairing under way carries on. */
void padpair_hidden(PadPair *p);

PairState padpair_state(PadPair *p);
GPtrArray *padpair_found(PadPair *p);        /* PairFound* */
const char *padpair_target(PadPair *p);
const char *padpair_reason(PadPair *p);

/* What the section says and what its button does, in each state. */
const char *padpair_action(PadPair *p);      /* NULL: no button now */
char *padpair_status(PadPair *p);            /* NULL: nothing to say */
