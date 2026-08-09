// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// Preferences.h — desktop shim. NVS becomes an in-memory map, so the harness
// can exercise settingsLoad()/settingsSave*() for real without a flash chip.
// Nothing persists between runs, which is what you want when experimenting.
#pragma once
#include <map>
#include "Arduino.h"

class Preferences {
 public:
  bool begin(const char*, bool = false) { return true; }
  void end() {}
  bool   getBool  (const char* k, bool d = false)        { auto i = b.find(k); return i == b.end() ? d : i->second; }
  int    getInt   (const char* k, int d = 0)             { auto i = n.find(k); return i == n.end() ? d : (int)i->second; }
  double getDouble(const char* k, double d = 0)          { auto i = f.find(k); return i == f.end() ? d : i->second; }
  String getString(const char* k, const char* d = "")    { auto i = s.find(k); return i == s.end() ? String(d) : i->second; }
  size_t putBool  (const char* k, bool v)   { b[k] = v; return 1; }
  size_t putInt   (const char* k, int v)    { n[k] = v; return 4; }
  size_t putDouble(const char* k, double v) { f[k] = v; return 8; }
  size_t putString(const char* k, const String& v) { s[k] = v; return v.length(); }
 private:
  std::map<std::string, bool>        b;
  std::map<std::string, long>        n;
  std::map<std::string, double>      f;
  std::map<std::string, String>      s;
};
