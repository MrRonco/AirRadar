# AirRadar — UI/UX Review

> **Status: PARTIAL.** The web-console review below is complete and verified.
> The 800×480 panel audit was stopped mid-run to conserve session budget; its
> per-lens findings are cached and resumable (see "Resuming" at the end).

## Scope correction — read this first

The commissioning brief described a **Slint/Rust** codebase. That is not what
this project is. There are no `.slint` files, no `.rs` files, no `Cargo.toml`,
and no Rust/Slint toolchain installed. AirRadar v7 is an **LVGL 8.3.11 + C++
(Arduino)** application under `firmware/AirRadar/src/ui/`, rendered by
LovyanGFX into a raw RGB565 framebuffer in PSRAM.

This review was performed against the real implementation. The renderer
constraints in the brief carry over almost unchanged (CPU-only software
renderer, no true backdrop blur, RGB565 banding, GT911 touch, ≥48 px targets);
the API names do not. If a Slint rewrite is genuinely planned, the materials
and motion findings would need restating — the hierarchy, typography, colour
and information-design findings would carry over as-is.

---

## Two defects found during review that are NOT design issues

Both verified directly in `firmware/AirRadar/src/svc/web.cpp`. Neither was
fixed in this pass (review-only), and both are worth acting on independently
of any redesign.

### D1 — The Wi-Fi form silently erases the stored password (HIGH)

`htmlAppendWifi` (web.cpp:161-168) renders `<input type=password name=pass>`
with **no value**, which is correct — a stored secret must never be echoed
into HTML. But `handleWifi` (web.cpp:276-290) then does:

```cpp
g_set.wifiPass = server.arg("pass");        // unconditional
g_prefs.putString("pass", g_set.wifiPass);
webReboot(...);                              // and immediately reboots
```

The SSID field *is* prefilled, so the natural action "fix a typo in the SSID,
click Save & reboot" submits an empty password, writes it to NVS, and reboots
into a device that can no longer join the network. Recovery is re-entering the
password on the on-panel 3-layer QWERTY keyboard.

The page already knows this pattern: MQTT URI and panel password both document
"blank = keep" (web.cpp:194, 199). The one field where blank-means-wipe costs
you remote access is the one field without it.

**Fix (3 lines):**
```cpp
String p = server.arg("pass");
if (p.length()) { g_set.wifiPass = p; g_prefs.putString("pass", p); }
```
Plus a label change to `Password (leave blank to keep current)`.

### D2 — The CSRF guard is bypassable by substring (MEDIUM-HIGH)

`authed()` (web.cpp:57) tests:
```cpp
if (o.length() && hostHdr.length() && o.indexOf(hostHdr) < 0) { ...reject... }
```
`indexOf` is a substring search. With `Host: airradar.local`, an attacker page
at `http://airradar.local.evil.com` sends a matching Origin, `indexOf` returns
7, and the guard passes. When no panel password is set, `authed()` returns true
for everything else, so this guard is the *only* protection on `/forget` and
`/update`.

**Fix:** compare exactly against `"http://" + hostHdr` (and an `https://`
variant), not by substring.

---

## Web UI (management console)

Reviewed against the served HTML (`frames/webui.html`, 2,849 bytes), a real 1280x900 desktop screenshot (`frames/webui-desktop.png`), and `firmware/AirRadar/src/svc/web.cpp` (778 lines). Everything asserted below was verified in one or both.

**One-line verdict:** this is a competent mobile settings form for a product that is neither mobile nor a settings form. The device already serves every number a first-class management console needs; the page displays exactly one of them (the version string). The fix is not more firmware — the data path is finished — it is spending ~2 KB of HTML on the half of the product that was never built.

### Scores

| # | Dimension | Score | One-line justification |
|---|---|---|---|
| 1 | Desktop layout & use of viewport | **2**/10 | `max-width:420px` wastes 67.2% of 1280px and 78.1% of 1920px; ~1,750px tall = 2.2 screens for one screen of controls |
| 2 | Information architecture & task flow | **4**/10 | Sane section order, but a flat linear scroll with no nav; the entire diagnostic surface is three footer links |
| 3 | Form design & input ergonomics | **4**/10 | Correct prefill and escaping; no `<label>`, no input types, no `box-sizing` (visible 20px misalignment), silent validation drops |
| 4 | Live state / observability | **1**/10 | Zero live values rendered. `/metrics`, `/api/state`, `/screen.bmp`, `/api/probe` all exist and all go unused |
| 5 | Visual identity vs the panel | **3**/10 | Different ink, different cyan, different type, different radius, no micro-labels, no altitude palette |
| 6 | Safety of destructive actions | **5**/10 | Real `confirm()` on all four consequential actions; but no danger zone, understated consequences, one dishonest reboot label |

**Aggregate: 19/60.** The single largest gap is dimension 4, and it is also the cheapest to close.

---

### CRITICAL

#### C1 — The console has no console

`handleMetrics` (web.cpp:653) already emits `in_range`, `heard`, `msg_rate`, `feed_local`, `wifi_rssi`, `heap_free`, `psram_free`, `heap_min`, `heap_largest`, `tls_shed`, `tls_conn`, `heap_delta_feeder`, `heap_delta_iss`, `uptime_seconds`. `handleApiState` (web.cpp:383) adds source name, weather, ISS, nearest, selected, and a full `flights[]` with route, registration, type and distance. `handleScreenBmp` serves the literal framebuffer. `handleApiProbe` runs a device-side LAN fetch test that settles firewall-vs-firmware arguments.

The management page surfaces **none of it**. Three `<a>` tags in the footer. `/metrics` opens as a wall of Prometheus text; `/api/state` as raw JSON; `/screen.bmp` downloads a 1.1 MB file.

**Fix:** header bar + eight-tile status strip above the fold, fed by `/metrics` alone at 5s. Parse in ~130 bytes:

```js
const m={};t.split('\n').forEach(l=>{if(l[0]!='#'){const[k,v]=l.split(' ');m[k]=+v}});
```

Gate on `document.visibilityState==='visible'`; `clearInterval` on `visibilitychange`. An abandoned tab must not keep hitting a webserver that shares `loop()` with LVGL.

Status colours, no greens: healthy `--cy` #54dcee, degraded `--amber` #f6b24a, failed `--red` #ff6472. Source pill mirrors the panel: LOCAL cyan / CLOUD dim / OFFLINE red.

**Cost:** +420 B markup, +760 B JS, +150 B CSS ≈ 1.3 KB, all static (flash-resident after H1). Per poll: `/metrics` reserves 896 B and runs ~14 `snprintf` — under 1 KB transient heap, 1-2 ms of loop. At 5s that is a 0.03% duty cycle.

#### C2 — 420px column on a desktop-only tool

Measured waste: **860px (67.2%) at 1280**, **1500px (78.1%) at 1920**. Page height ~1,750px, so "Forget Wi-Fi" is two scroll gestures away. There is even a `<meta name=viewport content='width=device-width'>` declaring the mobile intent in markup.

**Fix:** delete the viewport meta (−56 B, and a statement of intent). Then:

```css
*{box-sizing:border-box}
body{max-width:1240px;margin:24px auto;padding:0 20px}
.g{display:grid;grid-template-columns:repeat(12,1fr);gap:20px}
@media(max-width:1000px){.g{grid-template-columns:repeat(4,1fr)}}
```

That media query is the *only* small-screen concession; after it, stop thinking about phones. Result: header + status + live view above the fold at 900px (~580px used), one short scroll to settings, **~1,180px total vs 1,750px today**.

**Cost:** CSS 630 B → ~1,150 B (+520 B), +180 B wrappers. Zero runtime.

#### C3 — No charset declaration; non-ASCII SSIDs corrupt on round-trip

No `<meta charset>`, and `server.send(200,"text/html",h)` sends no charset parameter. Browsers fall back to locale default (often windows-1252) or sniff. SSID and feeder URL are echoed into `value='...'` from NVS, so `Café-IoT` renders as `CafÃ©-IoT` — and because the form POSTs whatever is in the field, submitting *any other form on the page* writes the mangled bytes back to NVS, after which the device cannot rejoin its own network. The panel's touch keyboard can produce non-ASCII, so this is reachable.

**Fix:** `<meta charset=utf-8>` first in `<head>`; `text/html;charset=utf-8` on the send. **22 bytes.** Highest value-per-byte fix on the page.

---

### HIGH

#### H1 — Build the page from flash, not from a heap String *(do this first; it unblocks everything)*

`handleRoot` reserves a 4,096-byte Arduino String and appends every `F()` literal into it — the whole page is copied flash → heap before a byte ships, then copied again by WebServer. Today that is a ~3 KB transient. But the TLS gate sheds optional fetches below a **45 KB internal-heap floor** and free heap runs 100-150 KB. Grow this to a real console at 12-15 KB and every page load becomes a 20-30 KB spike that can starve a concurrent enrichment fetch and bump `tls_shed` — the user sees blank routes and weather and blames the network. That coupling is the real reason this page has stayed small.

```cpp
server.setContentLength(CONTENT_LENGTH_UNKNOWN);
server.send(200, "text/html;charset=utf-8", "");
server.sendContent_P(kShellA);      // head + CSS + JS, straight from flash
server.sendContent(dynChunk);       // only interpolated values touch heap
server.sendContent_P(kShellB);
...
server.sendContent("");             // terminate chunked
```

**Cost:** ~60 lines net across the seven `htmlAppend*` functions. `kRootPageReserve` drops 4096 → 512. Peak render heap: ~3.5 KB → **under 1 KB, and flat as the page grows**. After this, the CSS/JS budget is bounded by the 3 MB app partition, not by RAM.

#### H2 — Runtime cost of the live surfaces, and the polling budget that follows

Two traps that will bite anyone building the dashboard naively.

**`/api/state` is a ~25 KB heap spike.** `DynamicJsonDocument doc(16384)` + an output String that grows to ~8 KB on a full 40-track feed, held simultaneously, in `loop()`, against the 45 KB TLS shed floor. Polling it every 2s = 30 spikes/minute colliding with feeder and enrichment TLS = rising `tls_shed` and blank routes — a UI decision manifesting as an apparent data bug.

**`/screen.bmp` freezes the panel.** `handleScreenBmp` (web.cpp:588) streams **1,152,054 bytes** as 480 sequential `halReadRect` + 2,400-byte `c.write()` pairs, entirely inside `loop()`. While it runs, `lv_timer_handler` does not, touch is not read, `applyPending` does not merge. The panel does **not** go black — RGB DMA keeps scanning the last framebuffer — it freezes and stops responding, for roughly **2-4 seconds** at the 2-4 Mbit/s a single ESP32-S3 stream of small writes realistically achieves (~0.9 s best case). Auto-refreshing at 5s is a 20-80% loop duty cycle to render a picture of a screen. No cache headers either, so browsers may serve a stale frame.

| Endpoint | Interval | Response | Transient heap | Notes |
|---|---|---|---|---|
| `/metrics` | **5 s** | ~700 B | <1 KB | the workhorse; drives the whole health strip |
| `/api/state` | **once on load, then 10 s** | ~8 KB | ~25 KB | only while Traffic is expanded **and** tab visible. Never < 5 s |
| `/screen.bmp` | **manual only** | 1,152,054 B | — | blocks `loop()` 1-4 s |
| `/api/probe` | **manual only** | ~80 B | <1 KB | blocks `loop()` up to ~6 s (2 s connect + 4 s read, web.cpp:636-637) |

**The real live view is not the BMP.** `/api/state` already carries `lat`/`lon`/`track_deg`/`alt_ft`/`dist_km` per flight plus home position and `range_km`. Draw a 240px plan-view on `<canvas>`: range rings, jet glyph rotated by `track_deg`, coloured by the panel's own altitude thresholds. **~1.5 KB of JS, ~8 KB per refresh — roughly 1,400× cheaper per view than the BMP.**

Keep `/screen.bmp` as an explicit diagnostic: thumbnail + Refresh button, cache-busted with `?t=`+`Date.now()`, labelled with its honest cost — *"1.1 MB · pauses the panel ~2s"*. If auto is offered at all: opt-in, 30s floor, killed on `visibilitychange` — and even then a 2s freeze every 30s is a 7% stutter the owner will notice.

If the mirror earns firmware: `?scale=2` with a 2×2 box average → 400×240×3 = 288,000 B, 4× less TCP time, freeze ~0.3-1 s. Note the handler already puts 4 KB on the stack (`row[800]` + `rowbuf[2400]`); a two-row averaging version needs a static or heap buffer, not more stack.

**Cost:** visibility guard ~180 B JS. Steady state with Traffic open: 12 `/metrics` + 6 `/api/state` per minute ≈ 56 KB/min on the wire, well under 1% loop occupancy. Collapsed: effectively zero.

#### H3 — Forget Wi-Fi has no danger zone and understates its own consequence

`htmlAppendFooter` (web.cpp:212) emits `<br>` then the red button — the two most dangerous controls on the page, adjacent, with the more dangerous one nearest your cursor after clicking Upload. The confirm reads *"Forget WiFi and reboot?"*: true, and useless. It never says remote access is gone, that this page and `/update` become unreachable, and that recovery means walking to the panel and retyping the password on the 3-layer touch QWERTY.

**Fix:** full-width danger zone as the last grid row, visually severed — `border:1px solid rgba(255,100,114,.35);border-radius:17px;padding:16px 20px;margin-top:36px`, `⚠ DANGER ZONE` micro-label in `--red`, consequence stated in one sentence. Replace `confirm()` with **type-to-arm**: an input that must match the current SSID before the button un-disables. Unbypassable by muscle memory, ~4 seconds of deliberate intent.

Also fix the honesty bug: the `/net` DHCP branch (web.cpp:294-303) only reboots when the device *was* static, yet both the button label and the confirm promise a reboot unconditionally.

**Cost:** ~450 B total. The SSID needs interpolating into the dynamic chunk (one extra `htmlEscape`).

#### H4 — `/api/config` GETs night-mode keys it cannot POST

`handleApiConfigGet` (web.cpp:430) returns `nighten`, `nightfr`, `nightto`. `apiCfgApply` (web.cpp:510) handles `lat lon rng feed lbl wxen issen logoen mapen tz mqtten mqtturi fcls faltlo falthi watch` — **not** the three night keys. `apiCfgValidate` does not reject them either, so `POST nighten=1` returns `{"ok":true}` and changes nothing. Any console rendering night mode from GET and POSTing it back ships a working-looking toggle that does nothing. Favourites (`fav0..2`) are in neither direction; `ppass` is write-only via `/integrations`.

**Fix before building any settings UI on this API:** either add the three keys to validate+apply (0-1439 minute range), or drop them from GET. Never ship a UI over an API whose write set is a strict subset of its read set. Until fixed, **do not render a night-mode control** — a dead toggle is worse than a missing one.

#### H5 — Settings coverage is backwards

Web exposes lat/lon/feeder/labels. The panel exposes night mode, four layer toggles, class filter, altitude filter, watchlist, favourites, range. `/api/config` already carries nearly all of it in both directions. The split ignores input modality — the things that are *miserable* on a 7" touchscreen are exactly the things missing from the web page.

| Tier | Settings | Rationale | POSTable today? |
|---|---|---|---|
| **1 — must be on web** | watchlist prefixes, POSIX TZ, feeder URL, lat/lon, MQTT URI, static IP, panel password | free text on a 3-layer touch QWERTY is punishing; a POSIX TZ string is effectively impossible | yes (except `ppass`, via `/integrations`) |
| **2 — cheap, add it** | range, alt filter lo/hi, class bitmask, `wxen`/`issen`/`logoen`/`mapen`, labels | the console is where you tune; a few selects and checkboxes | yes |
| **2 — blocked** | night mode | see H4 | **no** |
| **3 — deliberately omit** | favourites recall, target selection, range cycling | in-the-moment actions belong on the instrument you are looking at | n/a |

**Cost:** Tier 1+2 as plain fields into the three settings cards ≈ 1.1 KB static markup. Posting one JSON body to `/api/config` via `fetch()` adds ~350 B and buys inline validation plus a toast instead of a full navigation. `GET /api/config` on load = `DynamicJsonDocument(3072)` ≈ 4 KB transient, once. Negligible.

#### H6 — `blank = keep, - = clear` is implementation leaking into the label

Two labels literally read *"(blank = keep, - = clear)"*. The sentinel is undiscoverable, unmemorable, and **inverts expectations**: clearing the field to remove the broker produces a silent keep (web.cpp:349-354) — precisely the silent-swallow the house error-handling rule forbids. Compounding it, the current MQTT host is shown as a **placeholder**, so a configured broker looks identical to an unconfigured one.

- **Right fix (3 lines of C++):** `<input type=checkbox name=mqttclr value=1>Clear` beside the field; handler does `if(server.hasArg("mqttclr")) g_set.mqttUri=""; else if(uri.length()) ...`.
- **Zero-firmware fallback:** keep the sentinel, drive it from a Clear button — `onclick="f.mqtturi.value='-';f.mqtturi.style.color='#ff6472'"` — so no human ever types or knows about the hyphen.
- Either way, stop using placeholder as data display: render the redacted URI as `<div class=ml>CURRENT · mqtt://host:1883</div>` above an empty input, and **delete the parenthetical**. The UI should encode the rule, not explain it.

#### H7 — No OTA progress; every action dead-ends in unstyled plain text

A 1.5-3 MB `.bin` over WiFi takes 30-90 s with a frozen tab, a frozen panel, and nothing distinguishing "uploading" from "hung" — so the user reloads, which aborts the flash mid-write. Completion lands on `text/plain` "OK, rebooting" or a 500 with `Update.errorString()`: no styling, no link home, no indication of when the device returns. Same dead end for `/forget` and every validation 400.

```js
x.upload.onprogress=e=>{p.value=e.loaded/e.total*100;
  t.textContent=(e.loaded>>20)+'/'+(e.total>>20)+' MB'};
// and on every reboot notice page:
setInterval(()=>fetch('/metrics',{cache:'no-store'}).then(()=>location='/').catch(()=>0),3000);
```

That reconnect poller turns three dead ends into a completed round trip — the page reloads itself the instant the device answers. **Cost:** ~400 B OTA progress, ~140 B poller, ~90 B CSS. The poll only runs while the device is unreachable, so it costs the device nothing.

---

### NICE-TO-HAVE

#### N1 — Identity: closable to ~90% with zero web fonts

Ink #0b0f15 vs `C_INK` #05080d. Accent #4cc2ff vs `C_CY` #54dcee — close enough that the divergence reads as an accident. `system-ui` vs Inter/JetBrains Mono. Flat #151d28 inputs vs the `C_CARD_HI`→`C_CARD_LO` gradient with a 10%-opacity #b4cde6 hairline. 8px radius vs 17px. No micro-labels, no altitude palette, no glass.

You cannot ship Inter from a device with no internet — but you can **name it first**, so any machine that has it gets an exact panel match free, and everyone else falls back to metrically similar system faces. See the token block below.

#### N2 — Mechanical form defects

1. **No `box-sizing:border-box`** → `input{width:100%;padding:9px;border:1px}` computes to 440px inside a 420px column while `<select>` computes to 419px. Every input's right edge overhangs every select's by **20px**, visible throughout the screenshot. Fix: 22 bytes.
2. **No `<label>`** — bare text nodes, so clicking "Latitude" does not focus the field and assistive tech gets nothing. Fix: wrap as `<label>Latitude<input …></label>` — implicit association, no `id`/`for` pairs, ~15 B/field.
3. **No input semantics** — `type=number step=0.000001 min=-90 max=90` on lat, ±180 on lon, `type=url` on the feeder, `inputmode=numeric pattern="[0-9.]+"` on the four IP fields.
4. **Silent validation drops** — `handleSave` (web.cpp:249-265) discards out-of-range lat/lon and non-`http` feeder URLs and 303s home; the field snaps back with no message. Return a 400 naming the field, and validate client-side first so the round trip never happens.

**Cost:** ~430 B static HTML + ~8 lines of C++.

---

### Proposed desktop layout — 1280px

```
┌ 1280 viewport ──────────────────────────────────────────────────────────────────────┐
│ gutter 20              max-width 1240 · 12 col · 20px gap                    gutter  │
│ ┌──────────────────────────────────────────────────────────────────────────────────┐ │
│ │ ✈ AIRRADAR   airradar.local · 192.168.1.50     ● LOCAL 0s   up 3d 04:11   v7.0.0  │ │ 56
│ └──────────────────────────────────────────────────────────────────────────────────┘ │
│ ┌────────┬────────┬────────┬────────┬────────┬────────┬────────┬────────┐           │
│ │IN RANGE│ HEARD  │MSG RATE│ SOURCE │  RSSI  │  HEAP  │HEAP MIN│TLS SHED│           │ 84
│ │   8    │   8    │ 32 /s  │ LOCAL  │-58 dBm │ 118 KB │  61 KB │   0    │           │
│ └────────┴────────┴────────┴────────┴────────┴────────┴────────┴────────┘ /metrics 5s│
│ ┌───────────────── 8 col · 810px ─────────────────┐ ┌───── 4 col · 390px ─────────┐ │
│ │ TRAFFIC                       ⟳ 10s   ⏸         │ │ PANEL MIRROR                │ │
│ │  ┌──────────┐  FLT     TYPE  ALT   GS  DIST RTE │ │ ┌─────────────────────────┐ │ │
│ │  │   ·  ✈   │  ACA165  A320  FL285 455 119S YYZ→│ │ │   800x480 /screen.bmp   │ │ │ 400
│ │  │ ✈   ⊕  · │  GGN1049 C68A  4200  210  38E  —  │ │ │    (manual refresh)     │ │ │
│ │  │   ·   ✈  │  PAG372  …                        │ │ └─────────────────────────┘ │ │
│ │  └──────────┘  … scrolls                        │ │  ⟳ Refresh    ⧉ Full size   │ │
│ │  canvas 240²   /api/state 10s · paused if hidden│ │  1.1 MB · pauses panel ~2s  │ │
│ └─────────────────────────────────────────────────┘ └─────────────────────────────┘ │
│ ┌──── 4 col ───────────┐ ┌──── 4 col ───────────┐ ┌──── 4 col ──────────────────┐  │
│ │ RADAR & FEED         │ │ NETWORK              │ │ INTEGRATIONS                │  │
│ │ Lat  [51.470000]     │ │ Wi-Fi SSID [my-wifi] │ │ ☐ MQTT enabled              │  │
│ │ Lon  [-0.454300]    │ │ Password   [       ] │ │ CURRENT · mqtt://host:1883  │  │
│ │ Feeder URL [http://…]│ │       [Save & reboot⟳]│ │ Broker  [        ]  [Clear] │  │
│ │            [Probe ▷] │ │ ──────────────────── │ │ TZ  [EST5EDT,M3.2.0,M11.1.0]│  │ 430
│ │ Range  [250 km ▾]    │ │ Mode  [Static ▾]     │ │ Panel pw [      ]   [Clear] │  │
│ │ Labels ☑  Map ☑      │ │ IP    [192.168.1.50]  │ │              [Save]         │  │
│ │ Weather ☑ ISS ☑ Logo☑│ │ GW    [192.168.1.1]    │ │ ─────────────────────────── │  │
│ │ Alt filter [0]–[60000│ │ Mask  [255.255.255.0]│ │ FIRMWARE      current 7.0.0 │  │
│ │ Class ☑A ☑H ☑M       │ │ DNS   [192.168.1.1]    │ │ [Choose .bin] [Upload&flash]│  │
│ │ Watchlist [ACA,WJA]  │ │       [Save & reboot⟳]│ │ ▓▓▓▓▓▓▓░░░ 63% · 1.9/3.0 MB │  │
│ │            [Save]    │ │                      │ │                             │  │
│ └──────────────────────┘ └──────────────────────┘ └─────────────────────────────┘  │
│ ┌──────────────────────────────────────────────────────────────────────────────────┐ │
│ │ ⚠ DANGER ZONE                                              (1px --red @ 35%)     │ │
│ │ Forget Wi-Fi clears credentials and reboots. You will LOSE remote access — this   │ │ 96
│ │ page, /api and OTA all stop working — and must re-provision at the panel.         │ │
│ │                              type SSID to arm ▸ [          ]  [ Forget Wi-Fi ]   │ │
│ └──────────────────────────────────────────────────────────────────────────────────┘ │
│  AirRadar 7.0.0 · github.com/MrRonco/AirRadar · /api/state · /metrics · /screen.bmp  │
└──────────────────────────────────────────────────────────────────────────────────────┘

Above the fold at 900px: header + status + live row = ~580px.  Total ~1,180px (was ~1,750px).
⟳ marks every control that reboots the device.
```

### Design tokens — panel `theme.h` → CSS custom properties

```css
:root{
  /* palette — lifted verbatim from firmware/AirRadar/src/ui/theme.h */
  --ink:#05080d;      /* C_INK       page background            */
  --ink-hi:#0c1119;   /* C_INK_HI    gradient top               */
  --card-hi:#1c2838;  /* C_CARD_HI   card gradient top          */
  --card-lo:#0a1018;  /* C_CARD_LO   card gradient bottom       */
  --ivory:#eef1f4;    /* C_IVORY     primary text               */
  --ivory2:#aab4c0;   /* C_IVORY2    secondary text             */
  --dim:#69757f;      /* C_DIM       micro labels               */
  --faint:#39434e;    /* C_FAINT     dividers, disabled         */
  --cy:#54dcee;       /* C_CY        accent · live · focus ring  */
  --cy-soft:#3fb6c8;  /* C_CY_SOFT   hover                      */
  --amber:#f6b24a;    /* C_AMBER     alt <10k · warnings        */
  --violet:#a98cff;   /* C_VIOLET    alt >30k                   */
  --red:#ff6472;      /* C_RED       danger · errors            */
  --gold:#ffd77a;     /* C_GOLD      watchlist                  */
  --bord:rgba(180,205,230,.10);      /* C_BORDER @ OPA_BORDER 26/255 */
  /* geometry */
  --r:17px;           /* st_card radius                         */
  --r-s:9px;          /* control radius                         */
  /* type — Inter/JetBrains first so machines that have them match the panel exactly */
  --ui:Inter,-apple-system,BlinkMacSystemFont,"Segoe UI Variable Text","Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif;
  --mono:"JetBrains Mono",ui-monospace,SFMono-Regular,"SF Mono",Menlo,Consolas,"Liberation Mono",monospace;
}
body{background:linear-gradient(180deg,var(--ink-hi),var(--ink)) fixed;
     color:var(--ivory);font-family:var(--ui)}
.card{background:linear-gradient(180deg,var(--card-hi),var(--card-lo));
      border:1px solid var(--bord);border-radius:var(--r);padding:18px 20px}
/* the single most recognisable panel element — 85 bytes */
.ml{font:600 11px/1 var(--mono);letter-spacing:.12em;text-transform:uppercase;color:var(--dim)}
/* altColorRGB() semantics, reused on the flights table ALT column */
.a-lo{color:var(--amber)} .a-mid{color:var(--cy)} .a-hi{color:var(--violet)}
:focus-visible{outline:2px solid var(--cy);outline-offset:2px}
```

Token block ≈ 640 B, fonts ≈ 90 B, `.ml` ≈ 85 B, `.card` ≈ 120 B — **~940 B, all static, zero downloads, zero runtime cost.** `OPA_CARD` 216/255 is baked into the gradient rather than applied as `opacity`, since the web card sits on flat ink rather than over the map. **No greens anywhere** — healthy status is cyan, per the product-wide rule.

---

### Phased plan — ordered by value per byte of HTML

#### Phase 1 — "make it a console" · net +2.1 KB → page ~5.0 KB

Highest value per byte, and it is not close.

1. `<meta charset=utf-8>` + charset in Content-Type — **22 B** (C3)
2. **`sendContent_P` refactor** — do this before adding bytes; it makes every later byte free in RAM terms (H1)
3. Drop the viewport meta, 12-col grid, `box-sizing` — **+700 B** (C2, N2.1)
4. Design tokens + fonts + `.card` + `.ml` — **+940 B** (N1)
5. Header bar + 8-tile status strip on `/metrics` @ 5s with the visibility guard — **+1,330 B** (C1, H2)
6. Danger zone with type-to-arm; fix the `/net` reboot label — **+450 B** (H3)
7. Three-column settings wrap of the *existing* five forms — no new fields yet

**Result:** peak render heap drops from ~3.5 KB to under 1 KB despite the page growing 75%. The owner gets live aircraft count, feed source, message rate, heap and RSSI above the fold, in the panel's own visual language, with the destructive action properly fenced.

#### Phase 2 — "make it live" · +2.2 KB → page ~7.2 KB

8. Canvas plan-view from `/api/state` @ 10s, gated on panel-expanded + tab-visible — **+1.6 KB** (H2)
9. Flights table: FLT / TYPE / ALT (altitude-coloured) / GS / DIST / route — **+300 B**
10. `/screen.bmp` thumbnail, manual refresh, honest cost label, cache-bust — **+200 B** (H2)
11. OTA `<progress>` via XHR + reconnect poller on all reboot pages — **+630 B** (H7)

#### Phase 3 — "make it complete" · +1.9 KB → page ~9.1 KB

12. **Prerequisite:** close the `/api/config` night-mode read/write asymmetry (~12 lines of C++) (H4)
13. Tier 1+2 settings via `fetch()` to `/api/config` with inline validation and a toast — **+1.45 KB** (H5)
14. Replace the `-` sentinel with explicit Clear checkboxes (~3 lines of C++) — **+180 B** (H6)
15. `/api/probe` as a button next to the feeder URL with a spinner and a "pauses the panel ~6s" warning — **+280 B**

#### Optional phase 4 — only if the mirror earns it

16. `?scale=2` box-averaged BMP: 288 KB instead of 1.15 MB, freeze ~0.3-1 s instead of 2-4 s (~40 lines of C++, needs a static buffer — the handler already uses 4 KB of stack) (H2)

**Byte budget summary:** 2,849 B today → ~9.1 KB fully built out. Because phase 1 moves the static shell into `PROGMEM`, **peak internal heap per page render falls from ~3.5 KB to under 1 KB and stays flat** — a 3× larger, dramatically more capable page that is strictly *cheaper* in RAM than what ships today, and that never approaches the 45 KB TLS shed floor.

---

# Corrections applied to the section above

The web review was put through an adversarial verification pass. The following
claims in the section above were **refuted or amended** — trust these
corrections over the text above where they conflict.

| Claim above | Correction |
|---|---|
| Column is 420px; 67.2% of 1280px wasted; 78.1% of 1920px; 2.2 viewport heights | Column is **452px** (`max-width:420px` + `padding:0 1em`, no `box-sizing`). Wasted: **64.7%** at 1280px, **76.5%** at 1920px. Page is 1752px = **1.95** viewport heights. |
| Viewport meta is 56 bytes | **65 bytes** — and it is emitted a **second time** by `webReboot()` (web.cpp:121-122). Removing one without the other is a half-fix. |
| Missing `<meta charset>` is critical; other forms write mangled bytes back to NVS | **Downgrade to medium.** Each `<form>` submits only its own fields; `ssid` exists only in the `/wifi` form, so saving Settings/Network/Integrations cannot corrupt it. Real impact is on-screen mojibake, not NVS destruction. Still add the charset (22 bytes). |
| Refactor to `sendContent_P` is a **blocker** — growing the page costs 20-30 KB internal heap and starves TLS | **Refuted; demote to nice-to-have.** `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` routes any allocation >4 KB to **PSRAM**, and `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y` pins mbedTLS to internal RAM — they never compete. A 15 KB page costs *less* internal heap than today's. The byte budget is bounded by the 3 MB app partition, not RAM. |
| `/api/state` causes a ~25 KB internal-heap spike | **Refuted.** `DynamicJsonDocument(16384)` > 4096 → PSRAM. Real internal cost is single-digit KB. |
| `/screen.bmp` blocks `loop()` for 1-4 s | **Stands.** 480 sequential `halReadRect` + `c.write()` pairs in loop context; 1,152,054 bytes. Keep it manual-refresh only. |
| `/api/config` GET/POST asymmetry is 3 keys | **Nine keys**: `ssid, nstat, nip, ngw, nmask, ndns, nighten, nightfr, nightto` are returned by GET and handled by none of `apiCfgApply`. Also `mqtturi` is redacted on GET but written verbatim on POST — a GET→render→POST console silently destroys broker credentials. |
| Poll `/metrics` every 5s | **Use 10s.** `CONFIG_LWIP_MAX_ACTIVE_TCP=16` with `CONFIG_LWIP_TCP_MSL=60000`, against a feeder already opening ~30 connections/min, puts the device into continuous `tcp_kill_timewait` reaping. |
| Metrics parser one-liner | Has a bug: the body ends in `\n`, so the trailing empty line writes `m[""] = NaN`. Guard with `if(l && l[0] != '#')`. |

## Resuming the panel audit

Seven design lenses over the 800×480 panel completed and are cached; the
feasibility pass and the synthesis writer did not run. Resume with:

```
Workflow({
  scriptPath: "~/.claude/projects/-Users-francoraso-Documents-Development-Claude-AirRadar/8c89afa2-f562-4c8d-ae5c-4a54b92d287a/workflows/scripts/airradar-ui-ux-audit-wf_a3f05127-978.js",
  resumeFromRunId: "wf_a3f05127-978"
})
```

Cached lens output (raw, unsynthesised) is in that run's `journal.jsonl`.
Real captured frames used as evidence are in the session scratchpad under
`scratchpad/frames/`: `main-no-selection.png`, `main-selected-aircraft.png`,
`main-dense-traffic.png`, `webui-desktop.png`.
