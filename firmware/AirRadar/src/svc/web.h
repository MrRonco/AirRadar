// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// web.h — on-device web server: config UI, JSON API, live screenshot, OTA.
//
// Endpoints (all behind HTTP Basic auth "admin"/<panelPass> when set):
//   GET  /             settings page (lat/lon/feeder/labels + wifi + network +
//                      integrations sections; matches v6 features)
//   POST /save /wifi /net /forget      as v6 (validated, reboot where needed)
//   GET  /api/state    JSON: flights[], counts, nearest, source, rates, wx, iss
//   GET  /api/config   JSON: current settings (secrets omitted)
//   POST /api/config   JSON or form updates (validated field-by-field)
//   GET  /screen.bmp   live 800x480 screenshot (BMP24, streamed from panel FB)
//   GET  /metrics      Prometheus text (aircraft counts, rates, heap, rssi)
//   POST /update       OTA firmware upload (Update.h, app slot; reboots on OK)
//
// Security rules: validate every argument; never echo stored passwords;
// /update requires auth if a panel password is set (STRONGLY recommended).
#pragma once
#include "../core/state.h"

void webBegin();                    // start server + mDNS (call once WiFi is up)
void webLoop();                     // handleClient pump (every loop pass)
