// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// heapwalk.h — two-snapshot internal-heap diff for the leak hunt. Loop context.
#pragma once
#include <Arduino.h>

bool   heapWalkSnapshotA();   // baseline
bool   heapWalkSnapshotB();   // after the window
String heapWalkDiff();        // blocks in B and not A, with their contents
