// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "padconf.h"
#include "pads.h"

/* What runs while Lares is closed: connection notices, low battery alerts,
 * keeping the screen awake and disconnecting idle controllers. Ported from
 * Padlight's connectionNotices.js, batteryAlerts.js, keepAwake.js,
 * idleDisconnect.js and controllerActivity.js. Readers exist only while a
 * feature needs them, and wait on the device with no polling. */

typedef struct PadBg PadBg;
PadBg *padbg_new(Pads *pads, PadConf *conf);
