// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// Arduino.h — desktop shim. Just enough Arduino for the UI layer to compile and
// run in a window on a Mac; see desktop/README.md.
//
// Deliberately small. If a new firmware file needs more of Arduino than this
// provides, that is a signal worth reading: the UI is supposed to talk to LVGL
// and to g_* state, not to the platform.
//
// C-safe: LVGL's own .c files include this via LV_TICK_CUSTOM_INCLUDE, so
// everything C++ is fenced off below and only millis() crosses the line.
#pragma once
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_heap_caps.h"

#ifdef __cplusplus
extern "C" {
#endif
uint32_t millis(void);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <string>
#include <algorithm>
#include <chrono>

// ---------- String ----------
// Arduino's String over std::string. Only the methods the firmware actually
// calls are here — the compiler will tell you if that set grows.
class String {
 public:
  String() {}
  String(const char* s) : v(s ? s : "") {}
  String(const std::string& s) : v(s) {}
  explicit String(int n)    { char b[24]; snprintf(b, sizeof(b), "%d", n); v = b; }
  explicit String(unsigned n){ char b[24]; snprintf(b, sizeof(b), "%u", n); v = b; }
  explicit String(double d, int dec = 2) {
    char b[40]; snprintf(b, sizeof(b), "%.*f", dec, d); v = b;
  }

  const char* c_str() const            { return v.c_str(); }
  unsigned    length() const           { return (unsigned)v.size(); }
  bool        isEmpty() const          { return v.empty(); }
  void        reserve(unsigned n)      { v.reserve(n); }
  char        charAt(unsigned i) const { return i < v.size() ? v[i] : 0; }
  char        operator[](unsigned i) const { return charAt(i); }

  int  indexOf(char c, unsigned from = 0) const {
    auto p = v.find(c, from);  return p == std::string::npos ? -1 : (int)p;
  }
  int  indexOf(const char* s, unsigned from = 0) const {
    auto p = v.find(s, from);  return p == std::string::npos ? -1 : (int)p;
  }
  int  lastIndexOf(char c) const          { auto p = v.rfind(c); return p == std::string::npos ? -1 : (int)p; }
  String substring(unsigned a) const      { return a >= v.size() ? String() : String(v.substr(a)); }
  String substring(unsigned a, unsigned b) const {
    if (a >= v.size() || b <= a) return String();
    return String(v.substr(a, b - a));
  }
  bool startsWith(const char* s) const { return v.rfind(s, 0) == 0; }
  bool startsWith(const String& s) const { return v.rfind(s.v, 0) == 0; }
  bool endsWith(const char* s) const {
    size_t n = strlen(s);
    return v.size() >= n && v.compare(v.size() - n, n, s) == 0;
  }
  bool endsWith(const String& s) const { return endsWith(s.c_str()); }
  bool equals(const char* s) const          { return v == s; }
  bool equals(const String& s) const        { return v == s.v; }
  bool equalsIgnoreCase(const String& s) const {
    if (v.size() != s.v.size()) return false;
    for (size_t i = 0; i < v.size(); i++)
      if (tolower((unsigned char)v[i]) != tolower((unsigned char)s.v[i])) return false;
    return true;
  }
  void trim() {
    size_t a = v.find_first_not_of(" \t\r\n");
    size_t b = v.find_last_not_of(" \t\r\n");
    v = (a == std::string::npos) ? "" : v.substr(a, b - a + 1);
  }
  void toUpperCase() { for (auto& c : v) c = toupper((unsigned char)c); }
  void toLowerCase() { for (auto& c : v) c = tolower((unsigned char)c); }
  double toDouble() const { return atof(v.c_str()); }
  float  toFloat()  const { return (float)atof(v.c_str()); }
  int    toInt()    const { return atoi(v.c_str()); }

  String& operator+=(const String& o) { v += o.v; return *this; }
  String& operator+=(const char* o)   { v += o;   return *this; }
  bool operator==(const String& o) const { return v == o.v; }
  bool operator==(const char* o) const   { return v == o; }
  bool operator!=(const String& o) const { return v != o.v; }
  bool operator!=(const char* o) const   { return v != o; }

  std::string v;
};
inline String operator+(const String& a, const String& b) { return String(a.v + b.v); }
inline String operator+(const String& a, const char* b)   { return String(a.v + b); }
inline String operator+(const char* a, const String& b)   { return String(std::string(a) + b.v); }

// ---------- time ----------
inline void delay(uint32_t ms) { (void)ms; }   // the harness never blocks

// ---------- Serial ----------
struct SerialShim {
  void begin(unsigned long) {}
  void print(const char* s)   { fputs(s, stdout); }
  void println(const char* s) { fputs(s, stdout); fputc('\n', stdout); }
  void println()              { fputc('\n', stdout); }
  int  printf(const char* f, ...) __attribute__((format(printf, 2, 3)));
};
extern SerialShim Serial;

// ---------- FreeRTOS ----------
// On ESP32 these arrive via Arduino.h, so they live here too. The harness is
// single-threaded by design — it runs the loop-context half of the firmware,
// which is exactly the half the threading contract says owns all of LVGL — so
// the critical sections are genuinely no-ops rather than a simplification.
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m)  ((void)(m))
#define pdPASS 1
#define pdFAIL 0
typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);
// Spawning is refused, not faked: a net task that "succeeded" here would set a
// busy flag nothing ever clears, and the UI would wait forever for a result.
inline int xTaskCreatePinnedToCore(TaskFunction_t, const char*, uint32_t, void*,
                                   unsigned, TaskHandle_t*, int) { return pdFAIL; }
inline void vTaskDelete(TaskHandle_t) {}

// ---------- time-of-day ----------
// The real getLocalTime() blocks until NTP has produced a sane time. Here the
// Mac's own clock stands in, so the clock card and quiet-hours read correctly
// without a network.
#include <ctime>
// The firmware detects "no NTP yet" as tm_year <= 120 (i.e. before 2020). Host
// time always passes that test, so the unsynced branch was unreachable in the
// harness and its screenshot silently showed a normal clock. This flag makes
// the state reproducible.
extern bool g_fakeNtpSynced;
// updateTimeCard() calls time()/localtime_r() DIRECTLY and tests tm_year -- it
// never goes through getLocalTime() -- so intercepting only that helper left
// the unsynced branch unreachable. The macro is confined to the C++ section so
// LVGL's own .c files (which include this header for millis) are untouched.
extern "C" time_t ar_harness_time(time_t* t);
#define time(p) ar_harness_time(p)
inline bool getLocalTime(struct tm* info, uint32_t = 0) {
  const time_t now = g_fakeNtpSynced ? time(nullptr) : (time_t)0;
  localtime_r(&now, info);
  return g_fakeNtpSynced;
}
inline void configTzTime(const char*, const char*, const char* = nullptr,
                         const char* = nullptr) {}

// ---------- ESP ----------
// restart() is loud rather than silent: a harness that vanished on a Reboot tap
// would look like a crash, and the whole point of this thing is that you cannot
// brick it.
struct EspShim {
  void restart() { fputs("[harness] ESP.restart() ignored\n", stdout); }
  uint32_t getFreeHeap() { return 160 * 1024; }
};
extern EspShim ESP;

// ---------- odds and ends ----------
#ifndef min
template <class T> T min(T a, T b) { return a < b ? a : b; }
#endif
#ifndef max
template <class T> T max(T a, T b) { return a > b ? a : b; }
#endif
size_t strlcpy(char* dst, const char* src, size_t cap);

#endif  // __cplusplus
