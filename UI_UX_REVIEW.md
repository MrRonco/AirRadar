# AirRadar v7 — UI/UX Review

> Covers BOTH surfaces: the 800x480 on-device panel (sections 1-9) and the
> management web console (final section). Panel review verified against the
> real LVGL 8.3.11 source; web review verified against the live device.

## Part A — On-Device Panel

**Scope:** `SCR_MAIN` and the on-device screens only (`firmware/AirRadar/src/ui/`).
The management web console was reviewed separately and is deliberately out of scope here.
**Method:** seven audit lenses + a feasibility pass that checked every risky recommendation
against the actual LVGL 8.3.11 source in
`/private/tmp/.../scratchpad/toolchain/user/libraries/lvgl` and against `firmware/lv_conf.h`,
plus pixel measurements taken from three live framebuffer captures pulled off the device.

---

## ⚠️ Read this first — the commissioning brief described the wrong codebase

The brief this review was commissioned against describes a **Slint + Rust** application.
**There is no Slint and no Rust in this repository** — no `.slint`, no `.rs`, no `Cargo.toml`,
no Rust toolchain installed. The panel is **LVGL 8.3.11 + C++ (Arduino / esp32 core 3.3.10)**,
rendered by LovyanGFX into a raw RGB565 framebuffer in PSRAM.

Every finding below is expressed in LVGL 8.3 terms (`lv_obj`, `lv_style_t`, `lv_anim_t`,
`lv_img_dsc_t`, flex, `LV_DRAW_COMPLEX`) and every cost is quantified against the measured
hardware budget, not against a desktop renderer's.

**If a Slint rewrite is genuinely planned:** roughly 60% of this document (hierarchy, type
scale, palette, spacing, information design, feature gaps) transfers unchanged as design
intent; the other 40% — every cost model, the `LV_SHADOW_CACHE_SIZE 0` / `LV_OPA_MAX 253`
cover-check arguments, the flex-layout traps, the label-holder invalidation geometry — is
LVGL-specific and would need re-derivation against Slint's renderer, which composites very
differently.

---

## 1. Executive summary

- **The radar is not the hero of the radar.** Measured ink density on the idle frame ranks
  the 120,687 px scope disc **6th of 7 elements**. The `SETTINGS` button is 7.26× denser than
  the entire radar and its p95 luminance is 14.7× higher (0.3109 vs 0.0212). Chrome occupies
  151,616 px against the disc's 120,687 — the display gives **1.26× more area to furniture
  than to the product**. Everything else in this review is downstream of that inversion.
- **The instrument cannot be read at the stated viewing distance.** On this panel 1 px ≈
  1 arcmin at 650 mm. Scope callsigns (`F_MONO11`) are **8.0 arcmin** against the ISO 9241-303
  minimum of 16. The scope's own range scale is drawn in `C_FAINT` at a measured **1.85–1.95:1**
  contrast, and CARTO's baked city labels out-shout it. `config.h:57` says the geometry
  "matches the browser-verified mock 1:1" — a browser renders at 96 PPI, this panel is 133.3.
  **Everything was signed off 1.39× larger than it ships.**
- **There are three rendered defects on screen right now.** `315° NW+960` runs together with
  zero gap (`SEL_COL2_X 78` vs a measured 82.6 px string, `ui_cards.cpp:57,351`); the compass
  `N` is drawn underneath the weather pill and survives only as a ghost glyph fused into the
  wind icon (`ui_scope.cpp:481` vs `ui_cards.cpp:436`, z-order set by `ui_nav.cpp:29-30`); and
  the largest numeral on the display contradicts the line directly beneath it — **"11 IN RANGE
  / of 8 heard"** (`ui_cards.cpp:485` counts 60 s-coasted tracks, `:491` counts one poll).
- **You are paying real draw cost for effects that measure as no-ops.** The card box shadow
  produces a **1.009:1** luminance difference — invisible — for an uncached 3,362 B
  `lv_mem_buf_get` plus two box-blur passes **per card per repaint** (`LV_SHADOW_CACHE_SIZE 0`,
  `lv_conf.h:45`). `OPA_CARD 216` sits below `LV_OPA_MAX 253`, so every card returns
  `LV_COVER_RES_NOT_COVER` and every 1 Hz label tick recomposites the screen root beneath it —
  and it buys a **1.057:1** visual difference because no card overlaps the map. The page
  "gradient" quantises to **three colours with one hard 800 px seam at y=240**, straight
  through the scope's E-W crosshair.
- **The craft is real; the system is not.** The change-cache discipline, the holder-bounding
  comment block at `ui_scope.cpp:42-52`, the adaptive callsign resize, the hardware-verified
  flex note at `ui_settings.cpp:147` — this is genuine engineering. But there is no spacing
  scale (21 hand-placed constants, ten distinct step sizes in one 150 px column), no modular
  type scale (8 sizes, three of them inside an 18% band), 12 of 18 live numerics are set in
  **proportional** figures so the clock physically slides sideways every minute, and there is
  **zero touch feedback anywhere** — `grep LV_STATE_PRESSED` across the whole firmware returns
  nothing.

---

## 2. Scored rubric

Calibration: **10** = a shipped consumer instrument with a design team behind it.
**5** = competent in-house work. The competitor referenced by the owner (theqkash/esp32flight)
sits around **4**. This is scored against the owner's stated bar ("software that has had
millions in R&D"), not against the hobby-ESP32 field, where it would score much higher.

| # | Dimension | Score | One-line justification |
|---|---|---|---|
| 1 | Visual hierarchy & glanceability | **3**/10 | Scope ranks 6th of 7 by ink density; `SETTINGS` is 7.26× denser than the radar and 14.7× brighter at p95; the largest numeral contradicts the line under it. |
| 2 | Layout & composition | **4**/10 | Macro composition is sound (symmetric 204 px columns, centred scope) but only a meaningless 2 px "grid" fits all 20 layout constants; 8 of 20 fail a 4 px grid, 16 of 20 fail 8 px. |
| 3 | Typography | **4**/10 | Right families, disciplined 4bpp subsets (15.4 KB total), good adaptive callsign resize — undone by no modular ratio, three sizes inside 18%, and a measured 4.6 px column overrun shipping on screen. |
| 4 | Colour & contrast | **5**/10 | Palette is genuinely attractive and no-greens is honoured, but `C_FAINT` text measures 1.85:1, `C_DIM` 3.57:1, altitude is hue-only with non-monotonic luminance, and `C_RED` is the darkest semantic colour on screen. |
| 5 | Information design / data-ink | **4**/10 | The Selected card's route row is excellent; against that, a permanent 3-swatch legend teaching a code the owner memorised on day one, a 34 px void reserved for a strip hidden >99% of the time, and a 63,664 px empty card when nothing is selected. |
| 6 | Motion & animation | **3**/10 | Exactly one animation exists in the entire app (blip glide, `ui_scope.cpp:246-256`) and it is well judged; everything else is instant, and the one screen transition is a 220 ms **full-screen alpha composite** — the single largest momentary load in the firmware. |
| 7 | Touch interaction & affordance | **3**/10 | `LV_STATE_PRESSED` appears **nowhere** in the firmware — no control on any screen acknowledges a touch. Settings rows are 33 px (6.3 mm), favourite-delete buttons 28 px (5.3 mm), filter chips 24 px (4.6 mm), all under the 9 mm comfortable-touch minimum. |
| 8 | Consistency & system discipline | **4**/10 | Tokens exist in `theme.h` and are used, which is more than most projects manage — but the two tall cards share a style and share almost no internal rhythm (OV steps 20/24/26, SEL steps 46/46/24) and `±1`/`±2` eyeball nudges are scattered through `ui_cards.cpp`. |
| 9 | Craft & distinctiveness | **6**/10 | The best score here and it is earned: the route row, the logo tile, the operator monogram fallback, the coast/glide lifecycle and the change-cache discipline are all better than the field. It reads as a very good enthusiast build, not as a product. |
| 10 | Accessibility & legibility at 650 mm | **3**/10 | Only 3 of 8 faces clear the ISO 9241-303 16-arcmin floor; `F_M20` — which carries **every** primary instrument value — is 10% under it; the four smallest faces are 45-50% under; and the scale labels explaining the display are at 1.9:1. |
| | **Average** | **3.9**/10 | |

---

## 3. Prioritized findings

Feasibility labels: **[VERIFIED]** = checked against LVGL 8.3.11 source, buildable as written.
**[NEEDS_CONFIG]** = requires an `firmware/lv_conf.h` edit. **[NEEDS_ASSETS]** = requires a
font/image regeneration step. **[INFEASIBLE]** = do not attempt as specified.

### CRITICAL

---

#### C1. Attention inversion — the `SETTINGS` button is the brightest object on a radar **[VERIFIED]**

**Problem.** Ink density on `main-no-selection.png`: SETTINGS **0.05534**, TIME card 0.03078,
OVERVIEW 0.02895, weather pill 0.01957, range pill 0.01571, **SCOPE DISC 0.00763**, empty
SELECTED 0.00674. On the 11-target dense frame the scope only reaches 0.01085 and SETTINGS
still beats it 5.10:1. It earns this deliberately: it is the only object on `SCR_MAIN` with a
cyan-lifted background (`lv_color_mix(C_CY, C_CARD_HI, 46)`, `ui_cards.cpp:412`), a border at
opa 100 against `OPA_BORDER 26` for every data card (`:415` vs `theme.cpp:62`), and `F_M20`
ivory with `letter_space 2` at 14:1 contrast (`:423-425`). It is **12,144 px of permanently-lit
chrome displaying the same 8 characters forever**, next to the 120,687 px circle that is the
entire point of the device.

**Fix.** 44×44 icon-only gear, `LV_SYMBOL_SETTINGS` in `C_IVORY2` (**not** `C_DIM` — this is
the sole on-device route into `SCR_SETTINGS`, `ui_cards.cpp:206`, and `C_DIM` on `C_INK` is
only 4.26:1 for a 16 px glyph with no fallback gesture). Move it to the top-right corner at
(742, 8). Spend the cyan on `LV_STATE_PRESSED` instead of on rest state.

**LVGL mechanism.** New `st_iconbtn` in `theme.cpp` — **do not reuse `st_card`**:
`lv_style_set_shadow_width(&st_iconbtn, 0)` makes `lv_draw_sw_rect.c:437` early-return and the
3,362 B buffer plus both blur passes genuinely disappear. Keep `radius CARD_RADIUS` (17), **not**
`LV_RADIUS_CIRCLE` — a 138 px anti-aliased circumference of `C_BORDER` at opa 26 over `C_INK`
resolves to ~#171C23, which in 5/6/5 leaves a handful of distinguishable AA steps and visibly
stair-steps; the 17 px radius hides that in four short arcs. Press state:
`lv_obj_set_style_bg_color(btn, C_CY, LV_STATE_PRESSED)` +
`lv_obj_set_style_bg_opa(btn, 40, LV_STATE_PRESSED)` — `lv_obj_style.c:643` skips a
PRESSED-selector style at rest and `:649` returns it on exact match. Drop the redundant
`lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE)` at `:416`; `lv_obj.c:436` already sets it.

**Cost — honest version.** *Not* a throughput win. Nothing invalidates (602,400)-(786,466) at
steady state: the only `lv_obj_invalidate` on `SCR_MAIN` is the map image at `ui_scope.cpp:547`
and its bbox does not intersect the button. What this removes is a one-time 3,362 B
`lv_mem_buf_get` + two blur passes at screen build and on every return from settings, plus
~11,200 px of permanently-lit chrome and one undithered gradient. **Claim it as an attention
fix, not a perf fix.**

**Adjudication.** The hierarchy lens set the target as "the scope must rank 1st or 2nd by
density". That is arithmetically unreachable and it is the wrong objective — deleting SETTINGS
does not raise the scope's own density by one unit, and a sparse disc on near-black *is* the
correct design for a radar. **Replace the target with:** no non-data element on `SCR_MAIN` may
exceed **p95 luminance 0.10**; the only pixels above that are target glyphs, the count numeral
and the clock. Under that rule SETTINGS (p95 0.3109) is the single worst offender.

---

#### C2. The scope's own instrumentation is illegible and its north is hidden **[VERIFIED]**

**Problem.** Three failures of the radar's own reference frame.

1. Range-ring labels are `C_FAINT #39434e` at `F_MONO11` (`ui_scope.cpp:429,432`). Measured
   against the map floor (median Y = 0.00532): **1.89:1**; against the brightest map pixel,
   **1.38:1**. WCAG's floor for *non-text graphics* is 3:1. This is 11 px text.
2. `scopeBuildRingsAndCross` draws **three** rings (`ui_scope.cpp:415-417`) and
   `scopeUpdateRangeLabels` labels **two** (`:437-443`). `RING_INNER_D` — 83 km at the 250 km
   setting, the "roughly overhead" zone — is an unexplained circle.
3. The compass `N` is aligned at screen `y = SCOPE_Y0 - NORTH_LBL_GAP = 26` on the **screen
   root** (`ui_scope.cpp:481-483`). The weather pill occupies y 12..44 (`WX_PILL_Y 12`,
   `config.h:76`) and `cardsBuild` runs after `scopeBuild` (`ui_nav.cpp:29-30`), so the pill
   wins z-order. At 3× the top-centre crop reads `☀ 15° | [wind]N N 16 · Clear` — the compass
   survives as a ghost fused into the wind icon. **A radar with no visible north, in all three
   frames.**

Compounding all of it: CARTO `dark_all`'s own baked city labels ("Sudbury", "North Bay",
"Timmins") render **brighter** than our range scale. Third-party map furniture out-shouts the
instrument.

**Fix.**
- Promote ring labels from `C_FAINT` to the new `C_DIM #8e9baa`, at **`F_MONO13`** not
  `F_MONO11`, on a flat `C_INK` chip at **opa 200** (not 140 — measured, a 140 chip over the
  brightest map pixel gives only 2.97:1; 200 gives 3.75:1 worst case, 4.2:1 on median map).
- Label **all three** rings: 83 / 167 / 250 at the 250 km step. Arithmetic verified:
  `RING_INNER_D = 392/3 = 130` → radius 65 = 0.3316 R → 82.9 km. Sub-pixel agreement.
- **Delete** the screen-root `N` and `NORTH_LBL_GAP`. Recreate it as the last child of
  `s_clip` with `lv_obj_align(north, LV_ALIGN_TOP_MID, 0, NORTH_LBL_Y)` where
  `NORTH_LBL_Y = TICK_LEN + 3 = 15`. **Do not** hardcode `x = SCOPE_R - 4` — that centres a
  7 px glyph at x≈195.5, directly on both the 12 o'clock tick (`ui_scope.cpp:422`) and the
  full-height vertical crosshair (`:420`), striking a line through it. `LV_ALIGN_TOP_MID`
  self-centres regardless of glyph advance. The chip matters here specifically: it is what
  stops the crosshair bar from cutting the glyph.
- **Do not** "fix" this with `lv_obj_move_foreground(north)` after `cardsBuild` (the file
  already uses that idiom at `ui_scope.cpp:218-219`) — it would draw `N` *on top of* the
  weather pill, which is worse.

**LVGL mechanism.** Labels are non-clickable by construction (`lv_label.c:718` clears
`LV_OBJ_FLAG_CLICKABLE`), so chipped labels cannot steal taps from the `s_clip`
target-selection handler — this was the one silent way this change could have broken selection.
A label with `bg_opa > 0` draws a background because `lv_label_class.base_class = &lv_obj_class`
and `LV_EVENT_DRAW_MAIN` runs the base rect draw first. Add a shared `st_ringlbl` beside
`st_pill`/`st_microlbl` rather than four local style props per object (each
`lv_obj_set_style_*` reallocs that object's local prop array).

**Cost.** Everything inside `s_clip` already draws through the circular radius mask
(`lv_obj.c:565-568`), so the chip takes the same complex path the rings already take — one
34-byte mask line buffer, in PSRAM. Text is genuinely write-guarded: `scopeUpdateRangeLabels`
early-returns on `g_set.rangeKm == s_lastRangeShown` (`:437-438`). The only per-frame cost is
a blip holder gliding over a chip: worst case ~1,900 px/frame at 33 Hz = **63 kpx/s**, against
the 614 kpx/s that produced the documented wiggle. Unmeasurable.

**Note.** `RING_LBL_INNER_OFF` lands the third label at screen (436,202), inside the
near-overhead cluster visible in the dense frame. Survivable — range labels are created at
build time, before any blip holder, so aircraft always draw on top and never lose to the chip.
If it reads as broken, move all three to the **SW** diagonal (the emptiest quadrant in all
three captures) rather than splitting bearings.

---

#### C3. `315° NW+960` — a rendered value collision, shipping now **[VERIFIED]**

**Problem.** Measured from the real `adv_w` tables in `src/assets/font_m20.c`:

| string | font | width | cell | result |
|---|---|---|---|---|
| `315° NW` | F_M20 | **82.6 px** | 78 px (`SEL_COL2_X`) | **4.6 px overrun** |
| `36000 ft` | F_M20 | **83.7 px** | 78 px | **5.7 px overrun** |
| `17500 ft` | F_M20 | 77.4 px | 78 px | 0.6 px clear |
| `HEADING` | F_MONO11 +ls3 | 67.4 px | 78 px | 10.6 px clear |

`main-selected-aircraft.png` confirms it optically: `315° NW+960` with literally zero pixels
between the ivory and the cyan, and the two micro keys read as one word `HEADINGCLIMB`. The
format string is `"%03d\xC2\xB0 %s"` (`ui_cards.cpp:639`) and 4 of 8 cardinals are two letters,
so **roughly half of all headings collide**. The ALT cell has the identical bug below
`FL_TRANSITION_FT 18000` (`ui_cards.cpp:63`) — which fires for real traffic every day.

**Fix.** Three edits, all small, and note that **the layout is not what repairs this**:
1. Drop the redundant cardinal from the heading value. `315°` and `NW` carry the same
   information and the degrees are more precise. `315°` = 41.7 px at 20 px.
2. Move units into the key — standard instrument practice: `ALT ft` / `SPD kt` / `HDG` /
   `V/S fpm`, bare tabular numerals as the value. Lower `FL_TRANSITION_FT` to 10,000 so every
   ALT value is ≤5 glyphs.
3. Cut `st_microlbl` `letter_space` from 3 to 1 (`theme.cpp:85`). `HEADING` drops 67.4 → 52.4 px.
   273‰ tracking on a 6.625 px advance is ~4× the typographic maximum and destroys word shape.

**Adjudication.** The hierarchy lens proposed fixing this with a two-column flex-grow split
(150 = 71 + 8 + 71). **That makes both collisions worse before it fixes them** — 71 px is
*narrower* than the 78 it replaces, and 82.6 > 71, 83.7 > 71. Adopt the flex split for grid
discipline, but the typography above is doing 100% of the actual repair. Ship the typography
first.

**LVGL mechanism.** `snprintf` format at `ui_cards.cpp:625-629,639`; key strings at `:349-352`;
`lv_style_set_text_letter_space(&st_microlbl, 1)` at `theme.cpp:85`. Then guard this class of
bug permanently with `lv_txt_get_size(&sz, str, font, letter_space, 0, LV_COORD_MAX,
LV_TEXT_FLAG_NONE)` in a debug assert against the cell width — LVGL gives you the measurement
API; the design just never used it.

**Cost.** Zero flash, zero RAM, net win: shorter strings invalidate smaller rects on every 2 s
poll.

---

#### C4. The largest glyph on the display contradicts the line beneath it **[VERIFIED]**

**Problem.** `ui_cards.cpp:485` prints `g_orderN` at `F_NUM56` — the biggest type on the
screen, the designated hero number. `:491` prints `g_heardCount` directly beneath it as
"of N heard". `g_orderN` counts tracks in `g_tracks[]` that pass filters and sit within
`rangeKm` (`tracks.cpp:126-140`), and those persist through coasting for up to
`AR_DROP_TRACK_MS = 60000`. `g_heardCount` is `g_pendingHeard` from the **last single poll**
(`tracks.cpp:72`). Two different time windows presented as a fraction. `main-dense-traffic.png`
reads **"11 IN RANGE / of 8 heard"**. The element at the very top of the visual hierarchy is
the one the user learns to distrust, which discredits the card that owns it.

**Fix.** Hero = live tracks only (`age < AR_STALE_TRACK_MS`); second line = `"N COASTING"`
when N > 0 and **nothing** otherwise. Honest, uses state the code already computes, and removes
a line ~90% of the time. If "heard" is genuinely wanted, relabel to `"N in range · M msgs/poll"`
so it can never be parsed as a subset. The hero numeral must never be able to exceed the number
under it.

**LVGL mechanism.** Pure text change through the existing `setTextCached` path
(`ui_cards.cpp:151-156`) plus `setHiddenCached(s_ovHeard, coasting == 0)` using the helper
already at `:164-169`.

**Cost.** Zero — strictly fewer invalidations, because the second line disappears when there is
nothing to coast.

---

#### C5. Unknown altitude is pixel-identical to an emergency squawk **[VERIFIED]**

**Problem.** `core/state.cpp:255-260`:

```c
void altColorRGB(int altFt, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (altFt >= 30000)      { r = 0xa9; g = 0x8c; b = 0xff; }   // violet
  else if (altFt >= 10000) { r = 0x54; g = 0xdc; b = 0xee; }   // cyan
  else if (altFt >= 0)     { r = 0xf6; g = 0xb2; b = 0x4a; }   // amber
  else                     { r = 0xff; g = 0x64; b = 0x72; }   // unknown -> red-ish
}
```

`ui_scope.cpp:323` then does `col = emergency ? C_RED : lv_color_make(r,g,bl)`. `C_RED` **is**
`0xff6472`. An aircraft with no altitude renders **exactly** the same as a squawk 7700, modulo
a 500 ms blink. On a device whose one safety-adjacent signal is the emergency colour, that is
the most expensive possible colour collision.

Second half of the same problem: `C_RED` has relative luminance **Y = 0.316** — the **darkest**
of the four semantic colours. A 7700 squawk currently renders *dimmer* than every normal target.

**Fix.** Unknown altitude → `C_IVORY2 #aab4c0` (neutral, reads as "no data", already in the
palette). Emergency glyph → new `C_ALERT #ff8a94` (Y = 0.416, brighter than violet and close to
cyan) **plus** a 2 px `C_RED #ff6472` ring at `LV_OPA_COVER` around the holder, so an emergency
is the *largest* mark on the scope, not the darkest.

**LVGL mechanism.** Two branches in `state.cpp:255-260`; reuse the existing `makeRing()` helper
for the alert ring, toggled with the blip change-cache (`b.cInit` pattern, `ui_scope.cpp:324`).

**Cost.** Zero for the colour change. The alert ring is one 34×34 `lv_obj` per emergency blip,
hidden by default — no cost when nothing is squawking.

---

#### C6. Target labels are placed at a fixed offset with no collision handling **[VERIFIED — phase 1 only]**

**Problem.** Every blip label sits at exactly `(CLUSTER_PAD + LBL_OFF_X, CLUSTER_PAD +
LBL_OFF_Y) = (41, 13)` inside its holder — always right, always the same y
(`ui_scope.cpp:40-41,214`). The box is `LBL_W 104 × LBL_H 28`. At the 250 km setting 1 px =
1.28 km, so **any two aircraft within 133 km east-west and 36 km north-south produce overlapping
label boxes**. That is not an edge case in a TMA, it is the normal state.
`main-dense-traffic.png` proves it: `JZA233` and a second callsign composite into the
unreadable `JZ*EFEN` over the amber jet, and `CFC01` is clipped by the circle edge. Aggregate:
11 targets × 2,912 px = **32,032 px of label real estate demanded inside a 120,687 px disc —
26.5% of the radar claimed by text with zero arbitration.** Every label is identical
(`F_MONO11`, `C_IVORY2`, callsign only), so there is no way to tell which of eleven
equally-weighted strings matters.

**Fix — phase 1, do this one.** Cull, do not reposition. Show the callsign only for: the
selected target, watchlist matches (`trackOnWatchlist` already exists, `ui_scope.cpp:307`),
emergencies, and the nearest 3 by `g_orderIdx`. Dense frame goes from 11 labels to ~4,
collisions vanish in one change, ~20,000 px of competing text disappears.

**LVGL mechanism.** A predicate change inside `blipSetLabel` (`ui_scope.cpp:281-286`) feeding
the existing `setHidden` + `b.cLblHid` change cache. `g_set.showLabels` already gates this line
— extend the condition, add no objects.

**Cost.** Negative. Fewer visible labels = fewer drawn glyphs and a smaller effective dirty set.

**Phase 2 — 4-anchor placement: NOT free, budget it before you build it.** Bounding a label on
either side grows the holder from 145×41 to ~250×70 (17,500 px), a **2.9× increase in the rect
invalidated on every 900 ms glide**. With 11 gliding targets that is ~192,500 px per glide
cycle, against a budget where a single 4 Hz 392×392 map repaint (2.5 MB/s) already produced
visible shear. The holder must bound every child — `lv_obj_move_to` invalidates only the
holder's own rect, which is exactly the ghost-trail hazard documented at `ui_scope.cpp:42-52`.
**Ship phase 1; attempt phase 2 only with an on-device before/after wiggle check.**

---

### HIGH

---

#### H1. `OPA_CARD 216` + card shadows: slow draw path, no visual difference **[VERIFIED]**

**Problem — two measured no-ops.**

*Shadows.* `st_card` sets `shadow_width 24, shadow_ofs_y 10, shadow_opa 90`, black
(`theme.cpp:64-67`). Measured on the live frame: page background left of the Overview card
(x 2-13, y 150-250) has mean Y **0.00230** vs unshadowed far-left **0.00276** — a **1.009:1**
contrast ratio. Invisible. Above the card it is flat 0.00300 — literally zero shadow, because
`ofs_y` is +10. For that you pay, per card, per repaint: `corner_size = 24 + 17 = 41`, a
41×41×2 = **3,362 B `lv_mem_buf_get`** plus `shadow_draw_corner_buf`'s circle mask and two
box-blur passes, recomputed every time because `LV_SHADOW_CACHE_SIZE 0` (`lv_conf.h:45`)
compiles out the cache branch at `lv_draw_sw_rect.c:488-505`. Four cards. It also inflates each
card's invalidation rect by 26 px per axis (63,664 → 78,120 px, **+23%**) and pushes the left
card's shadow to x 211 and the right card's to x 589 — both **7 px inside** the scope clip
container, the one container that returns `LV_COVER_RES_MASKED`.

*Translucency.* `OPA_CARD 216` (`theme.h:27`) is below `LV_OPA_MAX 253` (`lv_color.h:58`), so
`lv_obj.c:511-513` returns `LV_COVER_RES_NOT_COVER` for every card. Every label repaint walks
down to the screen root and recomposites the page gradient underneath first — and labels like
`SOURCE LOCAL · 0s` (`:529`) and `LIVE · 0s` (`:668`) change **every second**. What the 216
buys: `theme.h:27` calls it "card body opacity (over the map edge)", but `config.h:70-71` puts
the cards at x 14..198 / 602..786 and the scope at 204..596 — **a 6 px gap; no card has ever
overlapped the map**. Measured card interior at (100,390) is (0,12,16), exactly
card-gradient-over-page, zero map contribution. The visible difference at 255 vs 216 is
**1.057:1**.

**Fix.** `bg_opa` → `LV_OPA_COVER`, `bg_grad_dir` → `LV_GRAD_DIR_NONE` with a single flat
`C_SURF #182231`, `shadow_width` → 0. Buy the elevation back with the hairline you already have:
`OPA_BORDER` 26 → 40. Result is a cleaner ladder than the gradient ever delivered: page
Y 0.00204 / card Y 0.01514 / inset Y 0.03362 — a constant 7.4× step from card top to card
bottom, instead of the current swing from 4.15× at the top to 1.47× at the bottom (which is
precisely why the empty Selected card reads as a *hole* rather than a panel in both frames).

**Adjudication.** The hierarchy lens argued for keeping `OPA_CARD 216` **if** the scope grows
enough to make the cards overlap the map. **Rejected.** An 18 px translucent sliver of map edge
is not worth a `NOT_COVER` full-stack recomposite under every ticking label at 4 Hz. My geometry
proposal (§5) grows the scope to R=212 **and** narrows the cards to 168 px so they are tangent
with an 8 px gutter — you get the bigger disc *and* the fast cover path. Take both.

**LVGL mechanism.** `theme.cpp:57-68` + `theme.h:27-28`. Verify with `LV_USE_PERF_MONITOR 1`
(`lv_conf.h:84`) before/after — that flag is already present, just set to 0.

**Cost.** The single best perf-for-free change in this review: −4 × (3,362 B transient alloc +
2 blur passes) per repaint, −23% invalidation area per card, and the NOT_COVER → COVER
transition removes a full-stack redraw beneath every ticking label at 4 Hz.

---

#### H2. The page "gradient" is a two-tone split with an 800 px seam through the crosshair **[VERIFIED]**

**Problem.** `makeRoot` sets `C_INK_HI #0c1119` → `C_INK #05080d` over 480 px
(`ui_nav.cpp:17-21`). In RGB565 that ramp spans R 1→0, G 4→2, B 3→1 — about three quantisation
steps total. Sampled down column x=5 on the live frame the actual output is **exactly three
colours**: `#081018` (y 0-1), `#000c10` (y 2-239), `#000808` (y 240-479). One band boundary,
800 px wide, at **y = 240** — two pixels below `SCOPE_CY 238`, landing on the E-W crosshair the
eye is already drawn to. `LV_DITHER_GRADIENT` is not defined in `firmware/lv_conf.h` so
`lv_conf_internal.h:389` defaults it to 0; there is no error diffusion to hide it. The third
colour also drifts hue — `#000808` is a teal-black, not the specified near-neutral.

The same quantisation hits the cards: sampled column x=100 through the left card shows G
stepping 32→28→24→20→16→12 and B stepping 41→33→24→16 — **seven horizontal Mach seams across a
346 px card**.

**Fix.** Delete both gradients. Flat `C_INK #05080d` page, flat `C_SURF #182231` card. That is
what 478 of 480 rows already show, it removes the seam, and it removes a gradient recompute
per screen-root repaint (`LV_GRAD_CACHE_DEF_SIZE` is at the LVGL default of 0, so
`lv_draw_sw_gradient.c:86` returns NULL and **every gradient is recomputed per draw**).

**Alternative, if a wash is genuinely wanted: `LV_DITHER_GRADIENT 1` — [NEEDS_CONFIG].**
Ordered dithering (`LV_DITHER_ERROR_DIFFUSION 0`) breaks the seam into noise that is invisible
at 133 PPI, but costs a per-row dither pass and an extra error/map buffer sized to the gradient
width. Given the screen root repaints under every NOT_COVER card today, flat is the safer
choice and composes with H1.

**Do NOT** solve this with a pre-baked 800×480 background image: 768 KB of PSRAM, and while
flash *reads* go through cache and are fine, any regeneration **writes** to flash, which stalls
the LCD DMA on the shared MSPI bus.

---

#### H3. Altitude is hue-only, and it costs a permanent legend **[VERIFIED — with a geometry warning]**

**Problem.** `C_AMBER` Y = 0.519, `C_CY` Y = 0.592, `C_VIOLET` Y = 0.344. Amber/cyan luminance
ratio is **0.88** — the same lightness, separable only on the blue-yellow opponent axis, which
is exactly the channel that degrades first in peripheral vision and under 5/6/5 quantisation.
The cost of hue-only encoding is a permanent legend: `mkRampBar` draws three bars plus three
`C_FAINT` labels pinned to the card bottom (`ui_cards.cpp:212-222,274-276`) carrying **342.7 ink
units = 7.2% of the whole screen's ink** — the second-largest single item in the Overview card —
to teach a code the owner memorised on day one. And the legend text is itself at 1.9:1, so the
key explaining the colour code is unreadable.

Worse, `C_CY` is simultaneously *interactive*, *live*, *altitude 10-30k* (`state.cpp:257`) and
*climb > 300 fpm* (`ui_cards.cpp:644`). Four meanings, one colour.

**Fix.** Add a second, luminance-carrying channel and delete the legend.
1. Re-tune the ramp to **monotonically descending luminance with altitude** (low = near =
   loud): `ALT_LOW #ffc061` (Y 0.599) / `ALT_MID #6fc7d8` (Y 0.491) / `ALT_HIGH #9b8ce0`
   (Y 0.321). Contrast against the map floor: 11.7:1 / 9.8:1 / 6.7:1. No greens (hues 38° /
   191° / 253°).
2. Scale the glyph with band: `lv_img_set_zoom` **300** (~1.17×) for <10k, **256** for 10-30k,
   **220** (~0.86×) for >30k. Low and close is physically bigger — pre-attentive, and it matches
   intuition.
3. Delete the ramp legend entirely (reclaims 36 px of the tallest card and 342 ink units) and
   print the FL directly under the callsign for the nearest 3.
4. Free `C_CY` from altitude duty entirely: cyan means **live data and things you just touched**,
   nothing else.

**LVGL mechanism.** `lv_img_set_zoom(b.jet, z)` with the existing
`lv_img_set_pivot(JET_SZ/2, JET_SZ/2)` at `ui_scope.cpp:210`. `LV_DRAW_COMPLEX 1`
(`lv_conf.h:44`) so image transform is compiled in. Guard with a per-blip change cache exactly
like `b.cAngle` (`ui_scope.cpp:332-335`) so an unchanged zoom never invalidates.

**⚠ GEOMETRY WARNING.** `CLUSTER_PAD` is 3 (`ui_scope.cpp:53`), sized so the 26 px glyph's
~37 px rotated diagonal overflows the 34 px cluster by only 1.4 px. **At 1.17× the rotated
diagonal is 43 px, so `CLUSTER_PAD` must go to 6 and `HOLDER_SZ`/`HOLDER_W`/`HOLDER_H` must
recompute with it** — otherwise the parent clips the glyph, because `LV_OBJ_FLAG_OVERFLOW_VISIBLE`
was deliberately removed (see the comment block at `ui_scope.cpp:42-52`). Holder grows 145×41 →
151×47, **+19% per-glide invalidation area.** Acceptable; budget it.

**Cost.** At 1.17× a 26 px glyph draws 30×30 = 900 px; 11 targets = 9,900 px per full-scope
repaint, 6% of one map repaint, and blips repaint only their own holder rect. Deleting the
legend is pure savings.

---

#### H4. 12 of 18 live numerics are proportional — the clock physically slides **[NEEDS_ASSETS]**

**Problem.** The generated font tables prove Inter's default figures are proportional.
`font_num36.c`: `'1'` adv_w 198 (12.38 px) vs `'4'` adv_w 348 (21.75 px) — a **9.4 px per-digit
swing**. The clock is `lv_obj_align(LV_ALIGN_CENTER)` (`ui_cards.cpp:399-400`), so `8:11`
(52.6 px) → `8:20` (69.1 px) shifts the whole string **8.2 px sideways every time a minute digit
changes width**; `1:11` → `12:44` is a 19 px swing. `font_num56.c`: `'9'` = 31.5 px, `'10'` =
52.5 px, so the IN RANGE label re-aligned at `ui_cards.cpp:487-489` **jumps 21 px** when the
count crosses 9→10. `font_ui15.c`: `'0'` 9.44 vs `'1'` 6.13 — and NEAREST distance, FEED and
SOURCE are all `LV_ALIGN_TOP_RIGHT`, so their **left edge breathes on every feed poll**. The
only stable values are the ones that happen to use JetBrains Mono (`font_mono11.c` has adv_w 106
for all 100 glyphs).

This is the loudest "hobby project" tell on the display. Real instruments do not have numbers
that wiggle.

**Fix.** Freeze Inter's `tnum` feature at build time and regenerate:

```
pyftfeatfreeze -f tnum InterDisplay-Light.ttf InterDisplay-Light-tnum.ttf
```

then run the existing `lv_font_conv` line in `firmware/tools/genassets.py` against the frozen
TTF for every numeric face. Where the value is a pure readout (DIST, SQK, CLIMB, FEED, SOURCE),
just move it to the JetBrains Mono face you already ship — tabular by construction. Then
**delete** the `lv_obj_update_layout` / `lv_obj_align_to` re-align hack at `ui_cards.cpp:487-489`
entirely: with tabular figures the count width is constant.

**Cost.** Flash neutral to +0.5 KB — tabular digits are the same bitmap count, and total font
bitmap flash today is only 15,436 B across all 8 faces against ~1.47 MB free in the 3 MB app
partition. **Perf is a net win**: a right-aligned proportional label that changes width
invalidates the union of old and new rects; a fixed-width tabular one invalidates a constant
rect.

---

#### H5. `F_MONO11` is 8.0 arcmin — half the ISO legibility floor **[NEEDS_ASSETS]**

**Problem.** 1 px = 25.4/133.3 = 0.1906 mm; at 650 mm, 1 arcmin = 0.1891 mm. **px ≈ arcmin.**

| face | cap height | arcmin | ISO 9241-303 min = 16′ |
|---|---|---|---|
| F_MONO11 | 8.03 px | **8.0′** | 50% under |
| F_UI12 | 8.7 px | **8.7′** | 46% under |
| F_MONO13 | 9.5 px | **9.5′** | 41% under |
| F_UI15 | 10.9 px | **10.9′** | 32% under |
| F_M20 | 14.5 px | **14.5′** | 10% under — **every primary instrument value** |
| F_L28 | 20.4 px | 20.4′ | clears |
| F_NUM36 | 26.2 px | 26.2′ | clears |
| F_NUM56 | 40.7 px | 40.7′ | clears |

Root cause is in the code's own comments: `config.h:57` — "matches the browser-verified mock
1:1", `ui_cards.cpp:2` — "Realizes the browser-verified 800×480 mock". A browser renders 800×480
CSS px at 96 PPI (a 211 mm-wide image); this panel is 133.3 PPI (152 mm wide). **Everything is
1.39× smaller in angular terms than it was when it was signed off, and nothing was re-tuned.**
On `main-dense-traffic.png` I had to upscale 3× nearest-neighbour before I could reliably
separate `ACA165` from `ACA143`.

**Fix.** Rebuild the small end on a ~1.22 ratio. Full scale in §6. Retire `F_MONO11`, `F_UI12`
and `F_MONO13` — three sizes inside 18% of each other that the eye cannot separate at this
distance, for ~4 KB of flash. Scope callsigns go to **15 px JBM** (cap 11′, +36% angular), and
pair that with content tiering: unselected targets show the callsign only, the two-line
`FL285 · 120 km` detail stays exclusive to the selected target. Then size `LBL_W` per state
instead of a fixed 104 (`ui_scope.cpp:36`) — the unselected holder gets **narrower** than today
while the text gets bigger, which also reduces collisions.

**Cost.** Flash +~4 KB net (retiring three faces returns 4.1 KB). Scope labels only change when
the callsign changes (change-cached at `ui_scope.cpp:303-306`), i.e. essentially never at steady
state, so this does not touch the continuous budget.

**Honest caveat:** card *keys* (`OVERVIEW`, `NEAREST`, `ALT ft`) at 15 px are still ~10.9′ and
below the ISO floor. That is acceptable — keys are recognition, not reading, and the owner has
memorised them. **Values must never be below the floor.** That is the line.

---

#### H6. `C_FAINT` measures 1.9:1 — the scale and legend are decoration, not information **[VERIFIED]**

**Problem.** Probed from the device framebuffer:

| element | colour | measured | required |
|---|---|---|---|
| Altitude legend `<10K / 10-30K / >30K` (`ui_cards.cpp:219`) | C_FAINT | **1.91:1** | 4.5:1 |
| Range-ring scale `250` / `167` (`ui_scope.cpp:429,432`) | C_FAINT | **1.95 / 1.85:1** | 4.5:1 |
| Micro keys (`st_microlbl`, `theme.cpp:84`) | C_DIM | **3.57:1** (card top) | 4.5:1 |
| `"Tap a target or swipe to cycle"` (`ui_cards.cpp:385`) | C_DIM | **3.83:1** | 4.5:1 |

Measured ink coverage inside those label boxes is 12-16%, so most of each glyph is fainter still
than the quoted peak. `C_FAINT` on text is below WCAG's 3:1 floor for *non-text graphics*, at
11 px.

**Fix.** Collapse the bottom two text steps into one that works and reserve real faintness for
genuinely non-semantic geometry. `C_DIM` `#69757f` → **`#8e9baa`** (5.65:1 on the flat card,
6.71:1 on the map, 7.35:1 on the page). **Retire `C_FAINT` as a text colour entirely** — keep
the hex only for decorative rings, where 1.7:1 is correct. Add `C_MUTE #75828f` (4.07:1) as a
fourth step for genuinely optional text, never for a scale.

**LVGL mechanism.** Two `#define` edits at `theme.h:15-16`; `lv_style_set_text_color(&st_microlbl, …)`
at `theme.cpp:84` propagates to every micro label automatically. The only hand-coloured call
sites are `ui_cards.cpp:219` and `ui_scope.cpp:429,432`.

**Cost.** Zero. LVGL renders a 4bpp glyph mask at any colour for the same cost.

---

#### H7. No spacing scale — 21 hand-placed constants **[VERIFIED — with four rules the naive version gets wrong]**

**Problem.** `ui_cards.cpp:22-60` declares 21 absolute constants. Their deltas are
OV: 20,30,6,68,24,34,10,20,24,26 and SEL: 20,32,22,38,20,18,8,46,46,24 — **ten distinct step
sizes inside one 150 px column**. Grid test on the 20 distinct values: 20/20 snap to 2 px (an
artifact of evenness, not a grid), 12/20 to 4 px, 5/20 to 6 px, 4/20 to 8 px. The `±2` nudges at
`:264,269,272` and the `±1` at `:359` are the fingerprint of nudging until it looked right.
Second symptom: the emergency strip reserves `OV_EMERG_Y 148 + OV_EMERG_H 26` and is
`LV_OBJ_FLAG_HIDDEN` >99% of the time (`:258`), leaving a permanent **34 px void** — the largest
whitespace in `main-no-selection.png`.

**Fix.** 4 px base (`SP_1 4 … SP_8 32`, §6), then **stop hand-placing**: `lv_obj_set_flex_flow`
+ `lv_obj_set_style_pad_row`. The hidden-strip collapse is free and correct: `lv_flex.c:344,460`
both skip `LV_OBJ_FLAG_HIDDEN`, and `lv_obj_clear_flag` (`lv_obj.c:279-286`) re-dirties the
parent layout when HIDDEN clears, so `setHiddenCached(s_ovEmergBox, …)` keeps working unchanged
and the 34 px void disappears on its own.

**⚠ Four rules the obvious implementation gets wrong — these are the difference between free and
1.0 MB/s of tearing:**

1. **Fix BOTH dimensions on every flex child, not just width.** `lv_label_refr_text` calls
   `lv_obj_refresh_self_size` unconditionally (`lv_label.c:919`), and that function early-returns
   only when width **AND** height are non-`LV_SIZE_CONTENT` (`lv_obj_pos.c:619-626` — the guard
   is AND, not OR). `lv_label_class` defaults **both** to `LV_SIZE_CONTENT`, so
   `lv_obj_set_width(lbl, w)` alone still marks layout dirty on every text change. Use
   `lv_obj_set_size(lbl, w, LINE_H)`. The codebase already learned this the hard way — see the
   HARDWARE-VERIFIED comment at `ui_settings.cpp:147-150` where SIZE_CONTENT flex boxes clipped
   `"DHCP"` to `"CP"`.
2. **Copy the row idiom that already ships here.** `mkRow()` at `ui_settings.cpp:125-144`
   (`LV_PCT(100) × 33`, `LV_FLEX_FLOW_ROW`, `LV_FLEX_ALIGN_SPACE_BETWEEN`) is exactly right and
   already validated on hardware. Note `mkBox`'s default: `lv_obj_class.width_def = height_def =
   LV_DPI_DEF` (=130), **not** SIZE_CONTENT — every new row container must be explicitly sized
   or it silently becomes 130×130.
3. **`LV_FLEX_ALIGN_START` cross-place on any container holding a width-varying label.**
   `children_repos` (`lv_flex.c:498-533`) issues **two** `lv_obj_invalidate(item)` calls whenever
   `diff_x || diff_y`. With START, a width change gives `diff_x = 0` → zero panel traffic. With
   CENTER or END, every width change repaints. `s_ovSrc` (`LOCAL · 12s`), `s_ovNearD`, `s_selLive`
   and `s_selDist` all change width every tick. Get this wrong and whole-card invalidation is
   184×346 × 2 cards × 4 Hz ≈ **1.0 MB/s** on top of the panel's 25.4 MB/s — ~41% of the load
   that already caused visible shear — plus a shadow-corner rebuild and a 346-entry gradient LUT
   rebuild per repaint.
4. **Section gaps need a spacer, not `pad_top`.** LVGL 8.3 has **no margin property**
   (zero `margin` hits in `src/misc/lv_style.h`). Absorb section gaps into the hairline rows
   (give `mkHair` an explicit height of `SP_8` with the 1 px line as a bottom border) or insert
   a plain sized `lv_obj`. `lv_style_set_pad_all` does not touch `PAD_ROW`/`PAD_COLUMN`, so
   `st_card`'s `pad_all` will not collide with your `pad_row`.

**Also:** flex will silently break the manual re-align at `ui_cards.cpp:487-489` —
`lv_obj_align_to` ends in `lv_obj_set_pos`, and `lv_obj_refr_pos` bails on the first line for
layout-positioned children (`lv_obj_pos.c:630`). Delete it (H4 makes it unnecessary anyway).

**Cost when done right.** `flex_update` over ~12 children × 2 cards per tick: integer arithmetic,
**zero framebuffer traffic**, because `children_repos` only touches the panel when a child
actually moves. ~10-12 new row containers, ~1 KB of LVGL heap (which is `ps_malloc`,
`lv_conf.h:30`). Not "no new RAM", but negligible.

---

#### H8. Zero touch feedback anywhere, and three touch targets are undersized **[VERIFIED]**

**Problem.** `grep -rn LV_STATE_PRESSED firmware/AirRadar/src` returns **nothing**. Not one
control on any screen acknowledges being touched. On a capacitive panel with no hover state and
no cursor, press feedback is the *only* channel that confirms input was received — its absence
is why a missed tap feels like a broken device rather than a missed tap.

Touch target sizes at 0.1906 mm/px against the ~9 mm comfortable minimum:

| control | size | physical | verdict |
|---|---|---|---|
| Settings row (`ui_settings.cpp:134`) | 364×33 | 6.3 mm | marginal |
| Switch (`:176`) | 40×22 | 4.2 mm tall | **under** |
| Favourite delete (`:771`) | 28×28 | 5.3 mm | **under** |
| Filter chip (`:831`) | h 24 | 4.6 mm | **under** |
| Range pill | ~h 36 | 6.9 mm | marginal, **and no press state** |
| Wi-Fi list row (`:542`) | h 46 | 8.8 mm | good |
| Keyboard / Save (`:641,711`) | h 52 | 9.9 mm | good |

**Fix.** One shared press treatment applied everywhere: `bg_opa 0 → 40` in `C_CY` on
`LV_STATE_PRESSED`, 90 ms `lv_anim_path_ease_out`. Raise settings rows to 40 px, chips and
favourite buttons to 40×40. That is the one place cyan should be spent — after which cyan on
`SCR_MAIN` means exactly two things: **live data, and the thing you just touched.**

**LVGL mechanism.** `lv_style_transition_dsc_init(&tr, props, lv_anim_path_ease_out, 90, 0, NULL)`
+ `lv_style_set_transition(&st, &tr)` on the **default** style so it applies in both directions;
`lv_obj_set_style_bg_opa(o, 40, LV_STATE_PRESSED)` per control or via a shared style.

**Cost.** A repaint of the control rect only, ~3 frames. The largest control is a 364×40
settings row = 14,560 px × 3 = 44 kpx per press. Nothing.

---

#### H9. The screen transition is a 220 ms full-screen alpha composite **[VERIFIED]**

**Problem.** `ui_nav.cpp:44`: `lv_scr_load_anim(s_roots[s], LV_SCR_LOAD_ANIM_FADE_ON, 220, 0,
false)`. `LV_SCR_LOAD_ANIM_FADE_ON` is an alias for `FADE_IN` (`lv_disp.h:38-39`) and it ramps
the incoming screen's opacity 0→255 while the outgoing screen stays. Every frame composites the
**full 800×480** with per-pixel alpha: 384,000 px × 2 B × ~7 frames at 30 ms refresh
(`LV_DISP_DEF_REFR_PERIOD 30`) ≈ **5.4 MB of blended traffic in 220 ms ≈ 24 MB/s**, on top of the
panel's continuous 25.4 MB/s DMA. This is almost certainly the single largest momentary load in
the firmware — and it buys a fade on a radar.

**Fix.** `LV_SCR_LOAD_ANIM_NONE`. Instant screen changes feel *more* responsive on a touch
panel, not less. If a transition is wanted for polish, `LV_SCR_LOAD_ANIM_MOVE_LEFT` at 160 ms is
strictly cheaper — it repaints opaquely rather than blending two full screens.

**Cost.** Negative. One enum change at `ui_nav.cpp:44`.

---

#### H10. The empty Selected card is a 63,664 px void — 16.6% of the display **[VERIFIED]**

**Problem.** When nothing is selected — which is the *default* state and the state in 2 of 3
captured frames — the right card holds one grey two-line string at density **0.00674**, dead
last of every element measured. A giant empty rectangle occupies the entire right sixth of a
radar display, and the instruction text (`"Tap a target or swipe to cycle"`, `ui_cards.cpp:385`)
is at 3.83:1.

**Fix.** Give it a job: show the **nearest 3** as a compact list (callsign / FL / km) when
nothing is selected. That converts the largest void on the display into the second-most-useful
surface, and it teaches the selection interaction **by example** instead of by instruction text.

**LVGL mechanism.** Three pre-built rows inside `s_selCont`, toggled with the existing
`setHiddenCached` helper (`ui_cards.cpp:164-169`) against `s_selEmpty`, populated from
`g_orderIdx[0..2]` which `tracksRebuildOrder` already maintains sorted by distance
(`tracks.cpp:126-140`). All texts through `setTextCached`.

**Cost.** 6 `lv_label` objects built once, ~1.5 KB PSRAM. Change-cached, so a stable sky costs
zero invalidations; a changing nearest-3 invalidates ~8,100 px at 4 Hz = **32 kpx/s**, which is
1.3% of the 614 kpx/s that caused the documented wiggle. Safe.

---

### NICE-TO-HAVE

---

#### N1. Bento grid: **rejected**, and the reason is as much perf as design **[VERIFIED]**

Bento grids solve N independent, roughly co-equal metrics competing for a rectangle. That is not
this screen. The Overview card is a single **ordered read** — identity, hero count, nearest, feed
health — where each row is subordinate to the one above. Fragmenting it flattens exactly the
hierarchy it depends on.

The cost is measurable: every additional tile is another `st_card` instance — another uncached
41×41×2 = 3,362 B shadow corner buffer with two blur passes, another gradient recompute, another
hairline, another 34 px of `pad_all`. **4 cards → 9 tiles multiplies the shadow work 2.25×.**
Geometrically it is worse: splitting a 184 px column 2-up leaves ~71 px of content per tile, and
`F_NUM56`'s `11` does not fit in 71 px. **You would spend a 2.25× draw-cost increase to make the
most important number smaller.**

Do apply one bento-adjacent move that *is* earned: the bottom row's two 184×66 slabs are
compositionally identical but semantically opposite — a passive clock and the only navigation
affordance on the screen. Split them (C1 + §5).

#### N2. `s_selEmpty` instruction copy

`"Tap a target or swipe to cycle"` is doing the job that H10's nearest-3 list does better. When
H10 ships, the instruction becomes a single 15 px `C_MUTE` line under the list
(`TAP A TARGET · SWIPE TO CYCLE`), not a centred paragraph occupying a 346 px card.

#### N3. Weather pill wind string reads `N N 16`

Partly C2's ghost glyph, partly that the pill prints a cardinal *and* a wind icon *and* a speed
with no unit. `N 16 kt` with the icon carrying "wind" is enough.

#### N4. `mkChevronValue` sets `lv_obj_set_width(v, 200)` inside a 222 px box (`ui_settings.cpp:154-160`)

Works, but the 200/222/7 triple is another un-tokenised hand-fit. Fold into the spacing scale
when §6 lands.

---

## 4. Where the lenses disagreed — adjudications

| Question | Lens A | Lens B | Ruling |
|---|---|---|---|
| Keep `OPA_CARD 216`? | Hierarchy: keep **if** the scope grows and cards overlap the map | Type/colour: go to 255 flat, no gradient | **255 flat.** Grow the scope *and* narrow the cards so they are tangent with an 8 px gutter — you get the bigger disc and the COVER fast path. An 18 px translucent sliver is not worth a NOT_COVER recomposite under every ticking label. |
| Fix the `315° NW` collision with flex-grow columns? | Hierarchy: yes, 150 = 71+8+71 | Type/colour: grow makes it **worse** (82.6 > 71) | **Typography first.** Drop the cardinal, units into keys, `letter_space` 3→1. Then adopt flex-grow for discipline. |
| Ring-label chip opacity | Hierarchy: 140 | Feasibility measured: 140 → 2.97:1 worst case | **200**, and `F_MONO13` not `F_MONO11`. |
| Gear shape | Hierarchy: `LV_RADIUS_CIRCLE` 44×44 | Feasibility: circle **raises** shadow cost (corner_size 41→46) and stair-steps in RGB565 | **44×44 at `CARD_RADIUS` 17, `shadow_width 0`.** |
| Gear colour at rest | Hierarchy: `C_DIM` | Feasibility: `C_DIM` on `C_INK` is 4.26:1 for the *only* route to settings | **`C_IVORY2`.** The ink reduction comes from killing the lit plate, not from dimming the glyph to the edge of legibility. |
| Label anti-collision | Hierarchy: cull, then 4-anchor placement | Feasibility: phase 2 is 2.9× glide invalidation | **Ship the cull. Gate phase 2 behind an on-device wiggle check.** |

---

## 5. Redesign direction

Design thesis, one line: **the disc is the product; everything else is a bezel.**
Chrome must lose area, lose luminance, and gain contrast where it carries meaning.

### 5.1 Current — 800×480, annotated

```
0        10        20        30        40        50        60        70      79
0┌───────────────────────────────────────────────────────────────────────────┐
 │                        ╔═══════════════════════╗                          │  wx pill y12..44
1│                        ║ ☀ 15° │ ≋ N 16·Clear  ║   ← eats the compass N   │  ← "N" ghost at y26
 │  ┌────────────────┐         ·····╪·····        ┌────────────────┐         │
2│  │ OVERVIEW       │      ····         ····     │ SELECTED       │         │  cards y46..392
 │  │ ⌂ Home         │    ···   Timmins     ···   │                │         │
3│  │                │   ··                   ··  │                │         │
 │  │  6  IN RANGE   │  ··      ✈ACA301        ·· │                │         │
4│  │                │  ·                       · │                │         │  ← 63,664 px
 │  │ of 6 heard  ←──┼──┼─ CONTRADICTS the 6      ·│                │         │    of nothing
5│  │                │ ··   ✈ACA343     ✈PTR2610 ·│   Tap a target │         │    (density
 │  │ ▓▓▓▓ 34px void │ ─┼───────────┼───────────┼─│   or swipe to  │         │     0.00674,
6│  │ (hidden strip) │ ··  Sudbury ✈PAG371  167  ·│   cycle        │         │     LAST of 7)
 │  │ ──────────     │  ·      ✈ETH574   250 ←─┼──┼─ C_FAINT 1.9:1 │         │
7│  │ NEAREST PAG371 │  ··    North Bay        ·· │                │         │
 │  │ 24.4 km NW     │   ··     ✈ASA459      ··   │                │         │
8│  │ FEED     27/s  │    ···              ···    │                │         │
 │  │ SOURCE LOCAL   │      ····        ····      │                │         │
9│  │ ▬▬▬ ▬▬▬ ▬▬▬ ←──┼───── 342 ink units of      └────────────────┘         │
 │  │ <10K 10-30K>30K│      legend at 1.9:1                                  │
0│  └────────────────┘                                                       │  band y400..466
 │  ┌────────────────┐      ┌───────────┐      ┌────────────────────────┐    │
1│  │      8:31      │      │ ‹ 250 KM ›│      │  ⚙  S E T T I N G S    │    │  ← 0.0553 ink/px
 │  │  WED · JUL 29  │      └───────────┘      └────────────────────────┘    │    7.26× the
2│  └────────────────┘  ↑ sits on the crosshair    ↑ brightest object on a     │    whole radar
 │   ↑ page gradient seam, 800px wide, at y=240        radar display          │
3└───────────────────────────────────────────────────────────────────────────┘
   disc 120,687 px (31.4%)   ·   cards 151,616 px (39.5%)   ·   ratio 1.26:1 WRONG WAY
```

### 5.2 Proposed — 800×480, exact numbers

```
0        10        20        30        40        50        60        70      79
0┌───────────────────────────────────────────────────────────────────────────┐
 │ ┌──────────────┐                                             ┌──┐         │  wx pill (12,8)
1│ │ ☀ 15°  N 16kt│              ·····N·····                    │⚙ │         │   168×36
 │ └──────────────┘          ···     │     ···                  └──┘         │  gear (742,8) 44²
2│ ┌──────────────┐       ···        │        ···       ┌──────────────┐     │
 │ │ ⌂ Home       │     ··           │           ··     │ ▣  ACA165    │     │  cards (12,56)
3│ │ WED · JUL 29 │    ·             │             ·    │    Air Canada│     │   & (620,56)
 │ │ ──────────── │   ·      ✈ACA301 │              ·   │ YYZ ──▶ YEG  │     │   168 × 344
4│ │              │  ·               │               ·  │ A320·C-FCUG  │     │
 │ │  8           │  ·         ✈     │      ✈        ·  │ ──────────── │     │  content 134×312
5│ │  IN RANGE    │ ·                │                · │ ALT ft SPD kt│     │
 │ │  2 COASTING  │ ·    ✈     83   167   250        · │ FL285   455  │     │  ring labels on
6│ │ ──────────── │ ├────────────────┼────────────────┤ │ HDG    V/S   │     │  C_INK chips
 │ │ NEAREST      │ ·          ✈JZA233 ←selected     · │ 315°   +960  │     │  opa 200, F_MONO13
7│ │ JZA233       │ ·                │                · │ DIST    SQK  │     │
 │ │ 10.4 km SE   │  ·               │               ·  │ 119.9km 2233 │     │  only 4 labels
8│ │ ──────────── │  ·        ✈      │     ✈         ·  │ ──────────── │     │  drawn: selected
 │ │ ● LOCAL·1s   │   ·              │              ·   │ ● LIVE · 0s  │     │  + watchlist
9│ │        24/s  │    ··            │            ··    │              │     │  + emergency
 │ └──────────────┘      ···         │         ···      └──────────────┘     │  + nearest 3
0│ ┌──────────────┐         ·····    │    ·····         ┌──────────────┐     │
 │ │  ‹  250 KM  ›│              ····╧····              │     8:31     │     │  band y412..464
1│ └──────────────┘                                     └──────────────┘     │   168×52
 │                                                                           │
2│                                                                           │
 │                                                                           │
3└───────────────────────────────────────────────────────────────────────────┘
   disc 141,196 px (36.8%, +17%)  ·  cards 116,256 px (30.3%)  ·  ratio 0.82:1 CORRECT
```

### 5.3 Exact geometry

| Constant | Now | Proposed | Why |
|---|---|---|---|
| `SCOPE_CX` | 400 | 400 | — |
| `SCOPE_CY` | 238 | **240** | true centre; kills the "2 px below the gradient seam" coincidence |
| `SCOPE_R` | 196 | **212** | disc 120,687 → 141,196 px (+17%); spans x 188..612, y 28..452 |
| `MAP_SIZE` | 392 | **424** | PSRAM 300 KB → 360 KB (+60 KB of 8 MB) |
| `CARD_W` | 184 | **168** | tangent to the disc with an 8 px gutter each side; **no overlap ⇒ no translucency needed** |
| `CARD_L_X` | 14 | **12** | 4 px grid |
| `CARD_R_X` | 602 | **620** | = 800 − 12 − 168 |
| `CARD_TOP_Y` | 46 | **56** | clears the 44 px gear at (742,8) |
| `CARD_TALL_H` | 346 | **344** | y 56..400 |
| card `pad_all` | 17 | **16** | 4 px grid; content 134×312 |
| bottom band | y 400..466 | **y 412..464, H 52** | two 168×52 slabs at x 12 and x 620 |
| `CARD_SHORT_H` | 66 | **52** | — |
| `WX_PILL_Y` | 12 (top **centre**) | **(12, 8), 168×36 top-left** | vacates the vertical axis; the compass gets clear air |
| `RNG_PILL_Y` | 446 (bottom **centre**) | **(12, 412), 168×52 stepper** | ‹ 42 px │ label 84 px │ › 42 px — all ≥ 8 mm |
| clock | (14,400) 184×66 | **(620, 412) 168×52** | date moves into the Overview header |
| settings | 184×66 lit plate | **44×44 icon at (742, 8)** | −11,200 px of lit chrome |
| compass `N` | screen root, y 26 | **inside `s_clip`, `LV_ALIGN_TOP_MID, 0, 15`** | can never be occluded again |

**Verification of the new geometry.** Disc at y=56 spans x 294.7..505.3 → clears both cards
(x ≤ 180, x ≥ 620) and the gear (742..786). Disc at y=412 spans x 276..524 → clears both bottom
slabs. Disc at its widest (y=240) spans 188..612 → 8 px gutter to each card. Gear y 8..52 clears
`CARD_TOP_Y 56` by 4 px. Bottom margin 16 px.

### 5.4 Card content — flex rows, exact sums

**Overview** (content 134×312; heights 225 + gaps 86 = 311 of 312):

```
h28  gap6   ⌂ Home                        22px Inter Medium   C_IVORY
h18  gap12  WED · JUL 29                  15px JBM +1         C_DIM
h1   gap12  ────────────────────────      hairline C_LINE opa 40
h56  gap2   8                             56px Inter Light tnum C_IVORY
h18  gap2   IN RANGE                      15px JBM +1         C_DIM
h18  gap12  2 COASTING   ← flex-collapses 15px JBM            C_MUTE
h1   gap12  ────────────────────────
h18  gap2   NEAREST                       15px JBM +1         C_DIM
h28  gap2   JZA233                        22px Inter Medium   C_IVORY
h20  gap12  10.4 km SE                    18px Inter tnum     C_IVORY2
h1   gap12  ────────────────────────
h18  —      ● LOCAL · 1s        24/s      15px JBM   dot C_CY
```

**Selected** (content 134×312; heights 246 + gaps 66 = 312 exactly):

```
h40  gap8   [logo 40²]  ACA165 / Air Canada   28px Inter Medium / 15px JBM C_DIM
h30  gap10  YYZ ──▶ YEG                       28px Inter Medium, C_CY when resolved
h18  gap6   A320 · C-FCUG · 2006              15px JBM  C_IVORY2
h1   gap6   ─────────────────────
h18  gap2   ALT ft        SPD kt              15px JBM +1 C_DIM   2 cols of 63, gutter 8
h28  gap8   FL285         455                 22px Inter Medium tnum
h18  gap2   HDG           V/S fpm
h28  gap8   315°          +960                22px tnum, C_CY/C_AMBER on strong climb/descent
h18  gap2   DIST          SQK
h28  gap8   119.9 km S    2233                22px tnum
h1   gap6   ─────────────────────
h18  —      ● LIVE · 0s                       15px JBM
```

Every value is now ≤5 glyphs at 22 px tabular (13.2 px/digit) → **max 66 px in a 63 px cell for
`FL285`**, which fits because `F` and `L` are narrower than tabular digits. `315°` = 47.6 px.
`455` = 39.6 px. The C3 collision cannot recur.

**Idle Selected card** (H10): rows 1-3 hidden, replaced by `NEAREST 3` + three
`callsign / FL / km` rows + one 15 px `C_MUTE` hint line.

---

## 6. Design tokens

### 6.1 Type scale — 6 faces (down from 8), ratio ≈ 1.22

| Token | px | Family / weight | Tracking | Tabular | Cap arcmin @650 mm | Role |
|---|---|---|---|---|---|---|
| `T_MICRO` | **15** | JetBrains Mono Medium | +1 px | **yes** (by construction) | 10.9′ | card keys, ring scale, status lines |
| `T_SCOPE` | **15** | JetBrains Mono Medium | 0 | yes | 10.9′ | target callsigns (same bitmap table as `T_MICRO`) |
| `T_BODY` | **18** | Inter Regular | 0 | — | 13.1′ | secondary text, distances |
| `T_VAL` | **22** | Inter Medium | 0 | **yes (`tnum` frozen)** | **16.0′** | every instrument value — **clears the ISO floor exactly** |
| `T_ID` | **28** | Inter Medium | 0 | — | 20.4′ | callsign, route, `Home` |
| `T_CLOCK` | **36** | InterDisplay Light | 0 | **yes** | 26.2′ | clock |
| `T_HERO` | **56** | InterDisplay Light | 0 | **yes** | 40.7′ | in-range count |

Retire `F_MONO11`, `F_UI12`, `F_MONO13` (three sizes inside an 18% band, invisible as a
hierarchy step at 650 mm, ~4 KB of flash). Keep `F_SYM16` as the `LV_SYMBOL_*` carrier.

Box-height tokens (set **both** dimensions on every flex child — see H7 rule 1):
`LH_15 = 20`, `LH_18 = 24`, `LH_22 = 28`, `LH_28 = 36`, `LH_36 = 46`, `LH_56 = 60`.

**Rule:** keys may sit below the ISO floor (recognition). **Values may not** (reading).

### 6.2 Spacing scale — 4 px base

`SP_1 4` · `SP_2 8` · `SP_3 12` · `SP_4 16` · `SP_5 20` · `SP_6 24` · `SP_8 32` · `SP_10 40`

Card padding `SP_4` (16). Intra-row gap `SP_1`/2 (2). Row gap `SP_3` (12). Section gap `SP_3`
before a hairline + `SP_3` after. Grid gutter `SP_2` (8). Screen margin `SP_3` (12).
**Every layout constant must be a multiple of 4. No exceptions, no ±1 nudges.**

### 6.3 Radius scale

`R_XS 4` (ring-label chips, filter chips) · `R_S 8` (inset tiles, logo tile) ·
`R_M 13` (settings groups — matches `ui_settings.cpp:107`) · `R_L 17` (cards, gear —
`CARD_RADIUS`) · `R_PILL LV_RADIUS_CIRCLE` (weather pill, range stepper, switches, LIVE dot).

**Never `LV_RADIUS_CIRCLE` on a small bordered rect** — a 138 px AA circumference of a low-opacity
hairline stair-steps visibly in RGB565 where a 17 px radius hides it in four short arcs.

### 6.4 Palette

| Token | Hex | Y | Role | Change |
|---|---|---|---|---|
| `C_INK` | `#05080d` | 0.0020 | page, flat — **no gradient** | grad removed |
| `C_SURF` | `#182231` | 0.0151 | card body, **flat, opaque** | replaces the `#1c2838`→`#0a1018` gradient |
| `C_SURF_HI` | `#26344a` | 0.0336 | inset (logo tile, ring chips use `C_INK`) | **new** |
| `C_LINE` | `#b4cde6` @ opa **40** | — | 1 px hairline; the only elevation cue | was opa 26 |
| `C_IVORY` | `#eef1f4` | 0.834 | primary text — 13.8:1 on card | keep |
| `C_IVORY2` | `#aab4c0` | 0.451 | secondary text; **also "no data"** | keep + new role |
| `C_DIM` | **`#8e9baa`** | 0.313 | keys, scale, labels — **5.65:1 on card** | was `#69757f` @ 3.57:1 |
| `C_MUTE` | **`#75828f`** | 0.212 | genuinely optional text only — 4.07:1 | **new** |
| `C_FAINT` | `#39434e` | 0.056 | **decoration only — never text again** | demoted |
| `C_CY` | `#54dcee` | 0.592 | **live data + pressed state ONLY** | freed from altitude/climb duty |
| `C_CY_SOFT` | `#3fb6c8` | 0.412 | group titles in settings | keep |
| `ALT_LOW` | **`#ffc061`** | **0.599** | altitude <10k · glyph zoom **300** | was `#f6b24a` |
| `ALT_MID` | **`#6fc7d8`** | **0.491** | altitude 10-30k · zoom **256** | was `C_CY` — collision removed |
| `ALT_HIGH` | **`#9b8ce0`** | **0.321** | altitude >30k · zoom **220** | was `#a98cff` |
| `C_ALERT` | **`#ff8a94`** | 0.416 | emergency **glyph** | was `#ff6472` @ Y 0.316 (darkest on screen) |
| `C_RED` | `#ff6472` | 0.316 | emergency **ring** (2 px, `LV_OPA_COVER`) + error states | role narrowed |
| `C_GOLD` | `#ffd77a` | 0.735 | watchlist | keep |
| `C_RING` | `#2b4d5d` | — | scope rings | keep |
| `C_RING_DIM` | `#193040` | — | crosshair | keep |

Altitude luminance is now **monotonically descending** (0.599 → 0.491 → 0.321): low = near =
loud, high = far = quiet, reinforced by glyph size. Contrast against the measured map floor:
11.7:1 / 9.8:1 / 6.7:1. **Hues 38° / 191° / 253° — no greens anywhere in this palette.**

### 6.5 Elevation

Three flat steps, **no shadows anywhere**:

| Level | Fill | Border | Measured step |
|---|---|---|---|
| 0 — page | `C_INK` flat | — | Y 0.0020 |
| 1 — card / pill | `C_SURF` flat, `LV_OPA_COVER` | 1 px `C_LINE` @ 40 | **7.4× over page**, constant top-to-bottom |
| 2 — inset | `C_SURF_HI` flat | 1 px `C_LINE` @ 26 | 2.2× over card |

`shadow_width 0` everywhere. Rationale is measured, not stylistic: the current shadow produces a
1.009:1 difference for a 3,362 B uncached buffer plus two box-blur passes per card per repaint,
and the flat 7.4× step is a *stronger* elevation read than the gradient's 4.15×-to-1.47× swing.
`LV_OPA_COVER` on every card restores the `LV_COVER_RES_COVER` fast path.

### 6.6 Motion

Total animation budget on this device is small — a 4 Hz repaint of one 392×392 image (2.5 MB/s)
alone produced visible shear. Every entry below is bounded and none runs continuously.

| Interaction | Property | Duration | LVGL path | Cost |
|---|---|---|---|---|
| Touch press (any control) | `bg_opa` 0→40, `C_CY` | **90 ms** | `lv_anim_path_ease_out` | control rect only, ≤14,560 px × 3 frames |
| Touch release | `bg_opa` 40→0 | **140 ms** | `lv_anim_path_ease_out` | same |
| Blip position glide | `x`, `y` | **900 ms** (`AR_BLIP_GLIDE_MS`) | `lv_anim_path_ease_out` | **unchanged — already correct** |
| Blip appear (new track) | `opa` 0→255 | **240 ms** | `lv_anim_path_ease_out` | one holder rect (151×47) |
| Blip drop (track lost) | `opa` →0 | **240 ms** | `lv_anim_path_ease_in` | one holder rect |
| Selection ring | `opa` 0→210 | **160 ms** | `lv_anim_path_ease_out` | 34×34 = 1,156 px × 5 frames |
| Emergency blink | `opa` 90↔255 | 500 ms half | keep the existing phase timer, **not** an `lv_anim_t` | one glyph |
| Screen change | — | **0 ms — `LV_SCR_LOAD_ANIM_NONE`** | — | replaces a 220 ms full-screen alpha composite (≈24 MB/s) |
| Range change | 3 ring `opa` pulse | **120 ms** | `lv_anim_path_ease_out` | 3 thin annuli only |

**Forbidden — these violate the budget and must not be added:**
- Any value-change flash or highlight on a card label (would invalidate a card at 4 Hz).
- Map crossfade on range change (two 424×424 buffers = +360 KB PSRAM and 179 kpx/frame blend).
- Any continuous sweep, pulse, or radar-sweep-line animation over the disc.
- `lv_style_transition` on anything larger than a single control.

Implementation: `lv_style_transition_dsc_init(&tr, props, lv_anim_path_ease_out, 90, 0, NULL)`
+ `lv_style_set_transition(&st, &tr)` on the **default** style so the transition applies entering
*and* leaving the state.

---

## 7. Feature gaps

| Feature | Value | Cost | Wow | Verdict |
|---|---|---|---|---|
| Label culling (selected + watchlist + emergency + nearest 3) | Removes the single worst legibility failure on the scope | ~20 lines in `blipSetLabel`; **negative** draw cost | 4 | **DO — phase 1** |
| Nearest-3 list in the idle Selected card | Converts a 63,664 px void into the 2nd-most-useful surface; teaches selection by example | 6 labels, ~1.5 KB PSRAM, 32 kpx/s | 4 | **DO — phase 1** |
| Altitude size channel (`lv_img_set_zoom`) | Makes altitude readable pre-attentively; kills the legend | ~15 lines + `CLUSTER_PAD` 3→6 (+19% glide area) | 3 | **DO — phase 4** |
| Emergency takeover (full-width alert strip + 2 px ring + largest glyph) | The one safety-adjacent signal is currently the *dimmest* thing on screen | ~40 lines; strip is flex-collapsed 99.9% of the time | 5 | **DO — phase 4** |
| Watchlist arrival banner (`trackOnWatchlist` already exists) | The feature exists but is invisible until you happen to look | ~30 lines, reuses the emergency strip | 4 | **DO — phase 5** |
| Route ETA + progress bar (`YYZ ▬▬▬▬▬▬░░ YEG`) | Turns a static route row into live information; adsbdb + gs + haversine, all already present | ~60 lines, one `lv_bar`, change-cached | 5 | **DO — phase 5** |
| Vertical profile strip (altitude vs distance, 134×60 in the Selected card) | Genuinely differentiating; nothing in the hobby field has it; all data present | ~120 lines on `lv_canvas` (`LV_USE_CANVAS 1` already set), 134×60×2 = 16 KB PSRAM, redrawn on selection change only | 5 | **DO — phase 5** |
| Breadcrumb trails (last 5 positions, fading) | Direction/history at a glance; v6 had this and v7 lost it | 5 `lv_obj` dots per tracked blip inside the holder — **watch the holder-bound rule at `ui_scope.cpp:42-52`** | 4 | **DO — phase 5, measure** |
| Per-band traffic counts in the freed bottom slab | Fills the space SETTINGS vacates with data | ~40 lines, `g_orderIdx` walk | 3 | MAYBE |
| Range auto-zoom to nearest traffic | Removes the most common manual interaction | ~30 lines; needs hysteresis or it thrashes the map fetch | 3 | MAYBE |
| Aircraft photos (planespotters) | High wow, but LVGL 8.3 ships **no JPEG decoder**; needs TJpgDec + a decode buffer + another TLS host under the 45 KB heap floor | high | 5 | **DEFER** |
| Airport / runway overlay | Needs a static airport DB in flash + projection work | high | 4 | PARK |
| Day/night terminator on the map | Solar geometry + per-pixel map tint = a full 424² recomposite | high | 3 | PARK |
| Session stats screen | Another screen to maintain for data nobody watches | medium | 2 | PARK |
| Dusk palette scaling (already on `docs/ROADMAP.md`) | Backlight is on/off only, so this is the only dimming available — trivial once colours are tokens | ~20 lines **if** §6.4 lands first | 2 | **DO — after phase 2** |

---

## 8. Phased implementation plan

Sized for LVGL on ESP32-S3 with a flash-and-inspect cycle after each phase. Every phase ends
with an on-device **wiggle check** (`LV_USE_PERF_MONITOR 1`, `lv_conf.h:84`, plus a visual
shear check at the 250 km range with >10 targets).

### Phase 1 — token surgery. Highest visual impact per line in the whole plan. (~1 day, ~120 lines, 5 files)

Nothing here changes geometry or layout structure. All of it is constants, colours and
predicates.

1. `theme.cpp:57-68` — `bg_opa` → `LV_OPA_COVER`, `bg_grad_dir` → `LV_GRAD_DIR_NONE`,
   flat `C_SURF #182231`, `shadow_width` → 0, `OPA_BORDER` 26 → 40. **(H1)**
2. `ui_nav.cpp:19-20` — delete the page gradient, flat `C_INK`. **(H2)**
3. `theme.h:15-16` — `C_DIM` → `#8e9baa`, add `C_MUTE`; retire `C_FAINT` from
   `ui_cards.cpp:219` and `ui_scope.cpp:429,432`. **(H6)**
4. `theme.cpp:85` — `letter_space` 3 → 1. **(C3)**
5. `ui_cards.cpp:406-427` — SETTINGS → 44×44 gear at (742,8), `st_iconbtn`, `C_IVORY2`,
   `C_CY` on `LV_STATE_PRESSED` only. **(C1)**
6. `ui_scope.cpp:428-443,481-483` — ring-label chips (`st_ringlbl`, opa 200, `F_MONO13`),
   third ring label, compass `N` moved inside `s_clip`. **(C2)**
7. `state.cpp:259` — unknown altitude → `C_IVORY2`; emergency glyph → `C_ALERT`. **(C5)**
8. `ui_cards.cpp:485-492` — hero = live only, second line = `N COASTING` or hidden. **(C4)**
9. `ui_cards.cpp:625-639` — drop the heading cardinal, units into keys,
   `FL_TRANSITION_FT` → 10000. **(C3)**
10. `ui_scope.cpp:281-286` — label culling predicate. **(C6 phase 1)**
11. `ui_nav.cpp:44` — `LV_SCR_LOAD_ANIM_NONE`. **(H9)**

**Expected result:** the three rendered defects are gone, the scope's scale becomes readable,
the brightest object on the screen becomes an aircraft, and the card repaint path moves from
NOT_COVER to COVER. Measure the wiggle before and after — this phase should *free* headroom that
later phases spend.

### Phase 2 — typography (~2 days, mostly toolchain)

Regenerate fonts with `tnum` frozen (`pyftfeatfreeze`) at the new sizes 15/18/22/28/36/56;
retire `F_MONO11`/`F_UI12`/`F_MONO13`; add `LH_*` box-height tokens; delete the
`lv_obj_update_layout`/`lv_obj_align_to` hack at `ui_cards.cpp:487-489`; sweep every
`lv_obj_set_width` on a label into `lv_obj_set_size`. **(H4, H5)**
Flash delta ≈ +4 KB against ~1.47 MB free. Highest risk of visual regression in the plan —
every string width changes at once, so re-measure the two cards against §5.4 after flashing.

### Phase 3 — layout system (~2 days)

4 px spacing scale; flex rows in both cards following the four rules in H7 (fix both dimensions,
copy `mkRow`, `LV_FLEX_ALIGN_START` cross-place on width-varying values, spacer objects not
`pad_top`); emergency strip auto-collapses; the 34 px void disappears. Then the geometry move:
`SCOPE_CY 240`, `SCOPE_R 212`, `CARD_W 168`, `CARD_TOP_Y 56`, pill relocation, bottom band
rebuild (§5.3). Re-tune the 3×3 CARTO stitch downscale for 424 px — tile fetch count is
unchanged at 9. **(H7, §5)**

### Phase 4 — motion, feedback and the alert path (~2 days)

Shared press transition on every control (H8); touch targets to 40 px minimum; blip
appear/drop/selection-ring animations (§6.6); altitude size channel with `CLUSTER_PAD` 3→6 and
the holder recompute (H3); emergency takeover treatment (§7). **Wiggle check is mandatory at the
end of this phase** — it is the first one that adds continuous work.

### Phase 5 — features (~3 days)

Nearest-3 idle list (H10); route ETA + progress bar; vertical profile strip on `lv_canvas`;
breadcrumb trails (measure the holder growth); watchlist arrival banner. Ship them one at a time
with a wiggle check between each.

**Total ≈ 10 working days.** Phase 1 alone closes every CRITICAL finding except C6-phase-2 and
delivers the majority of the perceived quality jump.

---

## 9. Infeasibility & prerequisite callouts

Everything in this document that is **not** a plain code edit, collected so nothing is silently
blocked.

### 🚫 INFEASIBLE — do not attempt as specified

| Item | Why |
|---|---|
| **Frosted glass / backdrop blur** | LVGL 8.3 has **no backdrop-blur primitive**. The only route is snapshot → downscale → box-blur, and `LV_USE_SNAPSHOT 0` (`lv_conf.h:86`) means it is not even compiled in. Turning it on costs a full-surface PSRAM snapshot (up to 768 KB) **plus** a CPU blur on every card repaint, against a budget where 2.5 MB/s of extra PSRAM traffic already caused visible shear. The flat `C_SURF` + hairline in §6.5 is the correct answer on this hardware, not a compromise. |
| **Map crossfade on range change** | Two 424×424 RGB565 buffers (+360 KB PSRAM) and a 179,776 px alpha blend per frame. Use the 120 ms ring pulse instead. |
| **Any per-frame animation over the disc** (sweep line, pulse, continuous glow) | The measured artifact threshold is ~614 kpx/s of extra PSRAM traffic. A full-disc redraw at 30 Hz is 4.2 Mpx/s — 7× over. |
| **Pre-baked 800×480 background image** | 768 KB PSRAM, and regeneration **writes** to flash, which stalls the LCD DMA on the shared MSPI bus. Flat fill is free and better. |
| **Aircraft photos (planespotters)** | LVGL 8.3 ships no JPEG decoder; requires TJpgDec integration, a decode buffer, and a fourth TLS host under the `AR_TLS_HEAP_FLOOR` 45 KB / `AR_TLS_BLOCK_FLOOR` 20 KB gate (`config.h:99-104`). Deferred, not rejected. |

### ⚙ NEEDS_CONFIG — requires editing `firmware/lv_conf.h`

| Item | Change | Needed by |
|---|---|---|
| Gradient dithering | `LV_DITHER_GRADIENT 1` (currently undefined → 0 via `lv_conf_internal.h:389`) | **Only** if you keep gradients. My recommendation removes them, so **this should not be needed.** Costs a per-row dither pass + an error/map buffer sized to the gradient width. |
| Perf instrumentation | `LV_USE_PERF_MONITOR 1` (`lv_conf.h:84`, currently 0) | Temporarily, for the before/after wiggle checks in every phase. Revert before shipping. |
| Shadow cache | `LV_SHADOW_CACHE_SIZE` > 0 | **Not needed** — phase 1 removes all shadows, which is strictly better than caching them. Listed only so nobody "fixes" the shadow cost by turning the cache on and paying internal SRAM for an invisible effect. |

⚠ Reminder: `firmware/lv_conf.h` must sit **beside** the lvgl library folder (see `CLAUDE.md`),
so any change here is a two-location change.

### 🎨 NEEDS_ASSETS — requires regeneration via `firmware/tools/genassets.py` + `lv_font_conv`

| Item | Detail | Flash delta |
|---|---|---|
| Tabular figures (H4) | `pyftfeatfreeze -f tnum` on InterDisplay-Light and Inter before `lv_font_conv`; adds a PyPI dependency (`opentype-feature-freezer`) to the asset toolchain | ~0 to +0.5 KB |
| New type scale (H5, §6.1) | Generate 15 (JBM Medium), 18, 22 (Inter Medium), keep 28/36/56; delete `font_mono11`, `font_ui12`, `font_mono13` | **+4 KB net** against ~1.47 MB free in the 3 MB app partition |

### 💾 EXTRA PSRAM

| Item | Delta | Against |
|---|---|---|
| `MAP_SIZE` 392 → 424 (`SCOPE_R` 212) | 300 KB → 360 KB, **+60 KB** | 8 MB OPI PSRAM — nil |
| Nearest-3 idle rows (H10) | ~1.5 KB | nil |
| Flex row containers (H7) | ~1 KB (10-12 objects, `ps_malloc` per `lv_conf.h:30`) | nil |
| Vertical profile canvas (§7) | 134×60×2 = **16 KB** | nil |

No item in this review increases **internal SRAM** pressure, which is the actually scarce
resource on this board (LVGL's heap is `ps_malloc`; the 45 KB TLS floor is the real constraint).

### ⚠ MEASURE-BEFORE-SHIP

| Item | Risk |
|---|---|
| Label anti-collision **phase 2** (C6) | Holder 145×41 → ~250×70 = **2.9×** glide invalidation; ~192,500 px per 900 ms cycle with 11 targets. Ship the cull first; gate this behind an on-device wiggle check. |
| Altitude glyph zoom (H3) | `CLUSTER_PAD` **must** go 3 → 6 with `HOLDER_*` recomputed, or the parent clips the rotated glyph (`OVERFLOW_VISIBLE` was deliberately removed). +19% glide area. |
| Flex adoption (H7) | Getting rule 3 wrong (cross-place CENTER/END on a width-varying label) costs ~1.0 MB/s of whole-card invalidation — ~41% of the load that already caused visible shear. |
| Breadcrumb trails (§7) | Every dot must stay bounded by the holder or it leaves ghosts on glide (`ui_scope.cpp:42-52`). |

---

*Reviewed against `firmware/AirRadar/src/ui/*` at commit `728b347`, LVGL 8.3.11, and three live
framebuffer captures. No source file was modified in the production of this document.*

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
