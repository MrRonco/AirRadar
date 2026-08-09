// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
#pragma once
enum { SCN_TYPICAL = 0, SCN_EMPTY, SCN_EMERGENCY, SCN_EXTREMES,
       SCN_CROWDED, SCN_COASTING, SCN_NOTIME, SCN_COUNT };
void        scenarioApply(int n);
const char* scenarioName(int n);
