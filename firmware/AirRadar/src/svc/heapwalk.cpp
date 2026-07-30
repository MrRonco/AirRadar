// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franco Raso
// heapwalk.cpp — identify the internal-SRAM leak by naming the leaked object.
//
// Byte counters told us bytes vanish; block counters told us the leaked object
// is ~1.1 KB and appears every ~10 s. Neither can say WHAT it is, which is why
// three previous hypotheses were guesses.
//
// heap_caps_walk() enumerates every block in the internal heaps with its
// address, size and used/free state. Take two snapshots, diff by address, and
// whatever is present in the second but not the first is what survived the
// window. Then dump the first bytes of each survivor: a task control block
// carries its task name in ASCII, an lwIP pbuf carries packet bytes, an Arduino
// String carries text. The contents usually name the owner outright.
//
// Snapshots live in PSRAM (8 MB free) so the measurement cannot perturb the
// internal heap it is measuring — the one mistake that would invalidate
// everything.
//
// Loop context only. Walking takes a heap lock, so keep the callback trivial:
// it records and returns, it never allocates or prints.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>
#include "heapwalk.h"

static const int kMaxBlocks = 2048;      // internal heap has ~500 live blocks

struct Blk { uint32_t addr; uint32_t size; };

struct Snap {
  Blk*     b;
  int      n;
  uint32_t uptime;
  bool     valid;
};

static Snap s_a = {nullptr, 0, 0, false};
static Snap s_b = {nullptr, 0, 0, false};

struct WalkCtx { Blk* out; int n; int max; };

// Trivial by design: this runs under the heap lock.
static bool walker(walker_heap_into_t, walker_block_info_t bi, void* ud) {
  WalkCtx* c = (WalkCtx*)ud;
  if (bi.used && c->n < c->max) {
    c->out[c->n].addr = (uint32_t)(uintptr_t)bi.ptr;
    c->out[c->n].size = (uint32_t)bi.size;
    c->n++;
  }
  return true;
}

static bool snapAlloc(Snap& s) {
  if (s.b) return true;
  s.b = (Blk*)heap_caps_malloc(sizeof(Blk) * kMaxBlocks, MALLOC_CAP_SPIRAM);
  return s.b != nullptr;
}

static bool takeSnap(Snap& s) {
  if (!snapAlloc(s)) return false;
  WalkCtx c = { s.b, 0, kMaxBlocks };
  heap_caps_walk(MALLOC_CAP_INTERNAL, walker, &c);
  s.n      = c.n;
  s.uptime = millis() / 1000;
  s.valid  = true;
  return true;
}

bool heapWalkSnapshotA() { return takeSnap(s_a); }
bool heapWalkSnapshotB() { return takeSnap(s_b); }

static bool inSnap(const Snap& s, uint32_t addr) {
  for (int i = 0; i < s.n; i++) if (s.b[i].addr == addr) return true;
  return false;
}

// Render up to `want` bytes of a block as hex + printable ASCII. Reading a live
// block is safe: it is allocated memory we merely inspect.
static void dumpBytes(String& out, uint32_t addr, uint32_t size, int want) {
  const uint8_t* p = (const uint8_t*)(uintptr_t)addr;
  int n = (int)(size < (uint32_t)want ? size : (uint32_t)want);
  char t[8];
  out += "    hex: ";
  for (int i = 0; i < n; i++) { snprintf(t, sizeof(t), "%02x ", p[i]); out += t; }
  out += "\n    txt: ";
  for (int i = 0; i < n; i++) {
    char ch = (char)p[i];
    out += (ch >= 32 && ch < 127) ? ch : '.';
  }
  out += "\n";
}

String heapWalkDiff() {
  String r;
  r.reserve(8192);
  if (!s_a.valid || !s_b.valid) {
    return F("need both snapshots: GET /api/heapwalk?a=1 then wait then ?b=1\n");
  }
  r += "snapshot A: t=" + String(s_a.uptime) + "s  blocks=" + String(s_a.n) + "\n";
  r += "snapshot B: t=" + String(s_b.uptime) + "s  blocks=" + String(s_b.n) + "\n";
  r += "window: " + String(s_b.uptime - s_a.uptime) + "s   net blocks: "
       + String(s_b.n - s_a.n) + "\n\n";

  // Survivors: in B, absent from A. These are what the window leaked.
  int shown = 0, survivors = 0;
  uint32_t bytes = 0;
  r += F("=== blocks present in B but not in A (leaked during the window) ===\n");
  for (int i = 0; i < s_b.n; i++) {
    if (inSnap(s_a, s_b.b[i].addr)) continue;
    survivors++;
    bytes += s_b.b[i].size;
    if (shown < 24) {
      char line[80];
      snprintf(line, sizeof(line), "  0x%08x  %5u B\n",
               (unsigned)s_b.b[i].addr, (unsigned)s_b.b[i].size);
      r += line;
      dumpBytes(r, s_b.b[i].addr, s_b.b[i].size, 48);
      shown++;
    }
  }
  r += "\nsurvivors: " + String(survivors) + " blocks, " + String(bytes) + " bytes\n";
  if (survivors) r += "mean size: " + String(bytes / survivors) + " B\n";
  return r;
}
