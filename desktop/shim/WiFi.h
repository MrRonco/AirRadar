// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// WiFi.h — desktop shim. Reports a plausible connected station so the settings
// screen and the status rows render their normal state. The scan returns a
// fixed list, which is far more useful than a real scan: the same networks
// appear every run, so the Wi-Fi screen's layout is reproducible.
#pragma once
#include "Arduino.h"

#define WL_CONNECTED 3
#define WL_IDLE_STATUS 0
#define WIFI_STA 1
#define WIFI_OFF 0
#define WIFI_SCAN_FAILED -2
#define WIFI_SCAN_RUNNING -1

struct IPAddress {
  uint8_t o[4] = {10, 0, 20, 161};
  IPAddress() {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) { o[0]=a; o[1]=b; o[2]=c; o[3]=d; }
  String toString() const {
    char s[16]; snprintf(s, sizeof(s), "%u.%u.%u.%u", o[0], o[1], o[2], o[3]);
    return String(s);
  }
  // Same acceptance rule as the real one: four decimal octets, nothing else.
  bool fromString(const char* s) {
    int a, b, c, d, n = 0;
    if (sscanf(s, "%d.%d.%d.%d%n", &a, &b, &c, &d, &n) != 4 || s[n] != 0) return false;
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return false;
    o[0] = (uint8_t)a; o[1] = (uint8_t)b; o[2] = (uint8_t)c; o[3] = (uint8_t)d;
    return true;
  }
  bool fromString(const String& s) { return fromString(s.c_str()); }
  operator uint32_t() const { return (uint32_t)o[0] | (o[1]<<8) | (o[2]<<16) | ((uint32_t)o[3]<<24); }
};

struct WiFiShim {
  int  status()                 { return WL_CONNECTED; }
  void mode(int)                {}
  void setSleep(bool)           {}
  void begin(const char*, const char*) {}
  void disconnect(bool = false, bool = false) {}
  IPAddress localIP()           { return IPAddress(10, 0, 20, 161); }
  IPAddress gatewayIP()         { return IPAddress(10, 0, 20, 1); }
  IPAddress subnetMask()        { return IPAddress(255, 255, 255, 0); }
  IPAddress dnsIP()             { return IPAddress(10, 0, 10, 8); }
  int  scanNetworks(bool = false) { return 5; }
  int  scanComplete()           { return 5; }
  void scanDelete()             {}
  String SSID()                 { return String("harness-net"); }
  String SSID(int i)            {
    static const char* n[5] = {"harness-net", "guest", "iot-vlan", "neighbour-2G", "printer"};
    return String(n[i % 5]);
  }
  int RSSI()                    { return -58; }
  int RSSI(int i)               { static const int r[5] = {-42, -58, -63, -77, -88}; return r[i % 5]; }
  int encryptionType(int)       { return 3; }
};
extern WiFiShim WiFi;
