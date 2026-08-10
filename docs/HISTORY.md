# Version history

## v1 — first light (inspired by Mirko Pavleski's CrowPanel ADS-B project)
Ported to the Waveshare panel: new RGB pin map, CH422G expander init, board settings
(16 MB / OPI PSRAM), UART-switch flashing fix. Worked, but the whole screen wiggled.

## v2 — ATC scope rewrite
Root-caused the wiggle: 40 fps sprite pushes + RGB DMA = PSRAM bandwidth contention,
plus WiFi modem-sleep bursts. Moved to event-driven rendering (persistent targets,
dead reckoning between polls, trails, coast/drop lifecycle) and `WiFi.setSleep(false)`;
`freq_write` 16→14 MHz. Rock solid. Green phosphor aesthetic, big 12h clock.

## v3 — touch edition
GT911 enabled (with controlled reset pinning I2C 0x5D — address otherwise randomizes
per power cycle). WiFi provisioning with scan list + 3-layer keyboard, NVS persistence,
tap/arrow aircraft selection with detail panel, coordinate numpad, gear menu, web
config page + mDNS. Deliberately no OpenSky credential screens (airplanes.live is
keyless; OpenSky OAuth2 + credit limits would be strictly worse).

## v4 — glass edition
Tech Talkies card layout, slate/cyan glassmorphism (per-pixel frost compositing into a
full-screen chrome sprite, built once at boot), FreeSans typography, amber/cyan/violet
altitude ramp. v4.1 added the local feeder: local-feeder-first polling at 2 s with
airplanes.live fallback at 8 s, `seen_pos` staleness guard, live source tag.

## v5
Symmetric card layout; age + source on separate lines; tappable range pill
(50/100/150/250 km, dynamic ring labels); local-time-only clock; NETWORK screen
(DHCP/static IP/GW/mask/DNS on-device, reboot-to-apply); bottom bar removed; web page
gained Wi-Fi and Network sections; stacked SELECTED/AIRCRAFT card title; centered
headers; numpad recentered; `<` backspace.

## v6 — layout redesign (immediate-mode, root AirRadar.ino)
Settings card removed from the radar; both columns mirrored top-to-floor; time moved
bottom-left, cog → full-width SETTINGS button; HOME in the Overview header; airplane
glyphs replacing arrows; emergency flash; the first satellite/CARTO base experiments.
Kept as the reference for proven data + hardware logic once v7 took over the UI.

## v7 — current: the LVGL rewrite
Full port to an LVGL 8.3 application (`firmware/AirRadar/`) chasing "past esp32flight":
anti-aliased Inter/JetBrains-Mono type, CARTO dark base map with city labels, animated
altitude-coloured aircraft glyphs, operator monogram + real airline logos (FATFS-cached),
adsbdb routes, Open-Meteo weather, ISS overhead, night mode, filters/favourites/watchlist,
Home-Assistant MQTT discovery, JSON API + `/screen.bmp` + `/metrics` + `/api/probe` + OTA.
Data core (feeder-first, dead-reckon, coast/drop) ported intact from v6.

First live hardware session settled a chain of physics: RGB panel wants byte-swapped 565;
LVGL draw buffer in internal SRAM but its heap in PSRAM; one TLS connection at a time or
mbedTLS starves; change-cache every widget write to stop redraw-vs-DMA wiggle; ISS on
plain HTTP to dodge an esp-tls leak. See `docs/V7_PORT.md` for the full findings, and
`firmware/BUILD.md` to build/flash. Installs one-click via `flasher/` (ESP Web Tools).

### v7.0.1 — the memory-starvation round
A second live session found the wiggle had returned *and* the device was silently
rebooting itself. Metrics forensics (plus a controlled quiet-vs-busy experiment, because
polling `/metrics` perturbs the very heap it reports) showed internal SRAM falling ~66 B/s
from boot, hitting a fragmentation cliff — 30 KB free but only a 10 KB largest block —
and latching at ~17 KB, permanently below the 45 KB TLS floor. Everything optional was
shed forever: no routes, no weather, no new logos. Airline logos still appeared, which is
exactly why the symptom looked like a data bug rather than a RAM bug — they load from the
FATFS cache with no TLS.

Four root causes, all distinct: (1) `setHidden()` wasn't change-cached and LVGL flag
writes invalidate unconditionally, repainting the whole map 4×/s — the wiggle;
(2) the 96 KB internal draw buffer was starving mbedTLS, and below the floor every
optional fetch still spawned a 12 KB-stack task just to find the gate shut, which was
itself the fragmentation engine; (3) `setSwapBytes(true)` silently disabled LovyanGFX's
per-row `memcpy`, making every flushed pixel an individual store into the PSRAM
framebuffer; (4) carrier names came only from the feeder's FAA/Transport-Canada database,
so foreign registrations had none — while adsbdb was returning `airline.name` in a
response we were already fetching and filtering out.

Result on hardware: free internal heap 17 KB → 159 KB, largest block 10 KB → 65 KB,
minimum-ever 124 B → 51 KB, weather and routes alive again. Routes also now walk the
visible list instead of resolving for the selected aircraft only.

### v7.1 — the design pass
Acted on an internal design audit, which scored the panel 3.9/10 against the
owner's brief. Headline finding: the radar was not the hero of its own display — the
SETTINGS button measured 7.26× denser and 14.7× brighter than the entire scope,
and chrome outweighed the disc 1.26 : 1 by area. It is now 0.82 : 1.

Geometry and type. Disc 196 → 212 px (+17% area); cards 184 → 168 px, tangent
with an 8 px gutter so they no longer overlap the circle and can stay opaque;
weather moved off its floating pill into the Overview card, which vacated the
vertical axis and made the compass-ghost bug structurally impossible; clock,
range stepper and a bare 26 px gear share one bottom band. Eight type faces
became six, values went 20 → 22 px to clear the 16-arcmin ISO 9241-303 floor at
650 mm, and tabular figures were frozen into Inter with `pyftfeatfreeze` so live
numbers stop shimmying as digits change.

Measured, not stylistic. Card shadows bought 1.009:1 of contrast for a 3,362 B
uncached buffer plus two blur passes per card per repaint — deleted. `OPA_CARD
216` forced `LV_COVER_RES_NOT_COVER`, recompositing the screen root under every
1 Hz label, for a 1.057:1 difference — cards are opaque. Dropping `clip_corner`
from the scope removed the same penalty again.

Safety. `altColorRGB`'s unknown-altitude branch returned `0xff6472` — byte
identical to `C_RED`, so an aircraft with no altitude was indistinguishable from
a 7700 squawk. Unknown is now ivory; red belongs to emergency alone, and the
ramp is luminance-ordered so it reads without a legend.

Caching, on the owner's suggestion. The stitched map only depends on
{lat, lon, range}, yet was re-fetched from CARTO every boot — 15 TLS handshakes
on the one subsystem that fails under heap pressure. Maps, logos and routes now
all persist to FATFS. The map is on screen 5 s after boot, before Wi-Fi
associates.

Web console. Rebuilt from a 452 px mobile settings form into a desktop console
with a live status strip and traffic table. Two defects found during the review
and fixed: `handleWifi` wrote an empty password over the stored one because the
field renders blank by design, and the CSRF guard was a substring match that
`http://airradar.local.evil.com` would pass.

## v7.2 — the measurement release

Two bugs that had each survived multiple wrong explanations, both closed by
building an instrument instead of arguing.

**The heap drain.** Free internal SRAM fell ~72 B/s from boot and never
recovered; after ~25 minutes the largest free block dropped below what an
mbedTLS handshake needs and every optional fetch was shed for the rest of the
boot. Cause: `vTaskDelete(NULL)` never returns, so a `DynamicJsonDocument`
declared in a task function never runs its destructor. `issTask` leaked 1,088 B
on every 15 s poll — 72.5 B/s against 72.7 measured. `wxTask` had it too, and
`routeTask` on four separate exit paths. Found by adding block-level heap
accounting (only ~0.2 new allocations per feeder poll against ~200 B lost, so
the leak was one ~1.1 KB object every 10–15 s, not many small ones), then
dumping surviving blocks with `heap_caps_walk` — whose contents read
`iss_position.latitude`, one per poll. Confirmed over a 21-hour soak at
0.23 B/s with zero shed fetches.

It hid for three sessions because `g_heapDeltaIss`/`g_issRuns` were declared,
published to `/metrics` and never incremented, so "ISS contributes exactly 0"
was a hardcoded zero read as a measurement.

**The display glitch.** `blipBuild` called `lv_obj_move_foreground()` twice per
new aircraft to keep the ISS marker on top. That resolves to
`lv_obj_invalidate(parent)`, and the parent is the 424×424 disc — so every newly
seen aircraft repainted the whole scope plus the Overview card, ~200,000 px and
~120 ms. The ISS is hidden except for a few passes a day, so almost all of it
reordered an invisible object. Worst repaint 53% → 11%, worst duration 129 ms →
60 ms. `LV_INV_BUF_SIZE` also raised 32 → 64, since overflow makes LVGL discard
every pending area and repaint the entire screen.

Three hypotheses died along the way — observer polling, TLS handshake load,
logo decode — each plausible, none measured. `/api/stalls` found it in two
readings. The lesson is rule 21.

Also in v7.2: the operator tile falls back to the ICAO code in the airline's
brand colour when no logo exists (general aviation, cargo, private), 65
verified carriers plus a deterministic hue for the rest; V/S no longer clips
(tabular figures make the minus sign a full digit wide, so "-3072" needed
71.2 px in a 64 px column); new diagnostics `/api/stalls` and `/api/heapwalk`.

## v7.2.1 — the shake

The remaining display glitch turned out not to be rendering at all. Every FATFS
write starves the panel DMA: flash and PSRAM share the MSPI bus, and
`CONFIG_SPI_FLASH_AUTO_SUSPEND` is disabled in the prebuilt arduino-esp32
libraries, so a write cannot be suspended to let cache traffic through. A
2,592-byte logo cost 222 ms of blocked bus; the 9 KB route table cost 327 ms.

The tell was the owner's own description — "the entire screen shakes" — which
distinguishes DMA starvation from a local repaint tear and eliminated six
hypotheses in one sentence. The measurable signature is a long stall with **zero
pixels flushed**, invisible to every repaint metric built up to that point.

Cost is fixed overhead rather than bytes, which means chunking a small write
makes it worse: copying the map's chunked pattern to the route table turned one
327 ms stall into four of ~150 ms, and was reverted. Frequency is the only lever
at these sizes. Route cache went to 15 minutes, logo saves to one per 45 s.
Measured over 7.6 hours: logo stalls fell from 4 per 5 minutes to 6 in the whole
run, route stalls to zero.

Also in v7.2.1: `WiFi.setSleep(false)` is re-asserted after every reconnect
(rule 2 was being applied once, at boot, while `setAutoReconnect(true)` silently
re-associates), and the stall detector gained the `rtcache` and `deadreck`
stages that had been blind spots.

## v7.2.2 — the weather row and the legend

No new subsystems; two pieces of the display that had drifted out of shape.

**The weather row.** The Overview card's top row is 136 px of card content and
it was carrying four things: a 22 px weather icon, the temperature in
`font_val22`, a 22 px wind glyph and the wind reading in `font_body18`. Measured
against LVGL's per-glyph rounding, the worst case wanted 187 px — so on an
ordinary day the degree sign sat on the wind glyph, and there was no arrangement
of those parts that fit. Both icons are regenerated at 16 px (`genassets.py`
emits from the same 22-unit drawing at a size constant, holding the stroke at
1.5 px of *output* so the pen does not thin as the glyph shrinks), and the
temperature drops to `font_body18`, which is what buys the room for both.

The row came out of it with one grammar: every reading is a VALUE in
`font_body18` and a QUALIFIER in `font_micro13` — 24 / °C, NW / 8. Baselines are
derived from each face's `line_height - base_line` rather than nudged, and the
icons centre on the body18 line box. The wind block's internal gaps are the only
thing not fixed: flat gaps are wrong at both ends, so they take what the two
readings leave behind, clamped to 2–6 px. Verified on hardware at both extremes,
including a probe forcing -40 / NW 120.

Also new: a **°C / °F toggle** (`tempf`, panel and web). It is a display
conversion only — `g_wx.tempC`, `/api/state`'s `temp_c` and MQTT stay Celsius,
because flipping a display preference should not change what the API means.

**The legend.** Five things were wrong at once. The scrim was 236/255, so the
clock, the range pill and any bright callsign read straight through the text.
"tap anywhere to close" was positioned at a hardcoded offset narrower than
"LEGEND" actually sets, and printed over the wordmark. The sample gutter was
34 px against a 40 px "FL350" sample. Every marker had its own left edge and its
own vertical nudge. And row pitch snapped to two buckets — 34 px for one line,
48 for anything else — which gave a three-line entry 9 px of air and a two-line
entry 22, the reason the columns looked ragged.

The scrim is opaque, the subtitle is measured from the title and sits on its
baseline, the gutter clears its widest sample by 10 px, markers share one edge
and centre on the first line of their description, and entries take their
measured height plus a fixed gap with the intra-entry line spacing set smaller
than that gap. Columns are even, there is a hairline under the masthead, and the
copy is tightened throughout. One entry added — the operator tile's
ICAO-in-brand-colour fallback, encoded since v7.2 and never explained, drawn
through `brandColorFor()` so the sample cannot drift from the real tile.

## v7.2.3 — the clock, and two things that were never tested by hand

**A 12/24-hour clock toggle** (`clk24`, panel and web, defaults to 12-hour so an
existing device looks exactly as it did). `font_clock36` carries digits and `:`
only — deliberately, it is a tabular clock face — so the 24-hour face needs no
new glyphs, but the meridiem cannot live in the same label. It is a separate
`font_micro13` label riding the clock's baseline, hidden entirely on the
24-hour face. Both offsets are derived rather than nudged: for a CENTER-aligned
label of height H the baseline sits at `H - base_line - H/2` below the parent's
centre, so matching the two faces is the difference of those. The pair
re-centres when the hour crosses one digit to two — "9:05" is 79 px and "12:05"
is 102.

**The range stepper's left chevron stepped up.** The pill draws `‹ 250 KM ›` but
the whole 168 px widget is one click target with one handler, and that handler
passed `+1` regardless of where the tap landed. Both arrows incremented, so the
only way to a shorter range was all the way round the cycle. `uiCycleRange(int
dir)` had always taken a direction — nothing ever called it with `-1` from the
pill. The tap is now hit-tested against the pill's midpoint.

**The `?` was stranded, and the touch zones were wrong.** 24 px of air between
the two marks where the cells they sit in are 10 px apart: both are centred in a
notional 26 px cell and neither fills it, so the cell gap and the visual gap
were never the same number. Position is now derived from the gap you see.

The touch layout underneath was worse than "spread out for safety". Two 26 px
buttons each with a symmetric 11 px `ext_click_area` gave a 48 px target apiece,
but those targets overlapped *each other* by 11 px and both reached 10 px down
into the Selected card — so taps near the card's top-right corner opened
Settings. Touch is now a separate pair of plates that tile the corner, meeting
in the middle of the visual gap and stopping at `CARD_TOP_Y`. Separating the
mark from the plate is the whole fix: centring a 12 px glyph in a 48 px target
is what forced the pair apart to begin with.

**Also tried and reverted: a precipitation radar overlay.** RainViewer's public
tiles are live and free, and the plumbing worked — disc-only composite into a
separate presentation buffer so the flash-cached map stays clean, 1.25 MB of
PSRAM only while switched on, partial invalidation so a frame change repaints
the disc rather than the panel. Two findings are worth keeping even though the
feature is gone. The public tiles stop at **zoom 7** — z8 and above return a PNG
that reads "Zoom Level Not Supported", which a naive fetch will happily paint
onto the scope. And the most common colour in the feed is *not* ground clutter,
as the first implementation assumed; it is the light-precipitation band, and
dropping it leaves a scatter of disconnected dots instead of weather. It was
removed because it did not look good enough on this display, not because it did
not work.

## v7.2.4 — the design review, and a tool to answer it with

Two reviewers went over every screen and returned 40 findings. The owner picked
36 of them. What made the batch possible at all was built first.

**The desktop harness** (`desktop/`). On the device, looking at a layout costs a
compile, an OTA flash, a reboot and a `GET /screen.bmp` — three minutes, with
the panel physically present and free. Seeing a state that is not currently
happening meant flashing firmware with the state hardcoded, and one of those
probe builds boot-looped the device badly enough to need USB recovery. The
harness compiles the real `ui_*.cpp`, `theme.cpp`, `tracks.cpp` and `state.cpp`
against SDL2 on a Mac, using the firmware's own `lv_conf.h` verbatim, so LVGL
rasterises the same `font_*.c` data and measurements taken there match the
device. Seven scenarios — including `-40 °C / NW 120 / -3072 fpm / FL450` and a
33-character operator name — render headless in under two seconds. It caught
two of this batch's own bugs before they reached the panel. It says nothing
about DMA, panel colour or touch; `/api/stalls` remains the only instrument for
those.

**A rank system for `font_micro13`.** One 13 px face was carrying thirteen
unrelated jobs on the main screen — grid keys, operator name, airframe,
registration, date, feed source, feed rate, scope callsigns, range value, IN
RANGE, NEAREST, the emergency string and the PM marker. Below 18 px all thirteen
have the same rank. Rather than add a font, the two axes already free were spent:
letter-spacing says what kind of thing a string is, colour says how much it
matters. `C_MUTE` had been defined and documented in `theme.h` since v7 and
never once used; it is the missing third rank.

**Hue stopped doing two jobs.** Watchlist gold `#ffd77a` and altitude amber
`#ffc061` are the same colour at two metres, twenty pixels apart. The watchlist
is marked by form now — brackets — and hue means altitude alone.

**The weather row became two tokens instead of one string.** It only reads as
two groups if the gutter between them beats the gaps inside them; the old code
spent its slack evenly on both wind gaps and fixed the gutter at 6, so at
`-40 °C / NW 120` every space on the row was between 2 and 6 px. Inner gaps are
fixed small, the gutter takes everything left, and when even that is not enough
the unit *letter* is dropped — the degree ring stays, and which unit you are in
is a setting you chose.

**The map became a ground.** `dark_nolabels` instead of `dark_all`: CARTO's
labelled tiles put place names at z9–z11 under a 424 px disc already carrying
glyphs, callsigns, three range numerals and a crosshair, and at reading distance
a 7 px town name is not legible, only textured. The tint lift dropped from ×1.6
to ×1.1 for the same reason.

That change also exposed a trap worth more than the change itself: the map cache
key was `lat,lon`, so any device with a warm cache would have kept serving the
**old** map forever. The fetch never runs, so there is no error and nothing to
notice. The key now carries `AR_MAP_RECIPE`.

**A graduated bearing scale.** The panel could always show you that something
was north-east; it could not tell you it was on 038. A tick every 10°, longer
every 30°, engraved as *pixels* into the map buffer after the coverage lens —
36 ticks in the clip container's child list would cost something on every disc
repaint (rule 19), while a pass over a 19,000 px annulus on core 0 at map-build
time costs nothing afterwards. Anti-aliasing is analytic rather than
supersampled. The four cardinals are deliberately skipped so the scope still has
its axes with the map switched off, and the orphaned "N" was promoted: it now
sits inside the ring with E, S and W at one radius, four marks reading as one
system instead of one mark reading as a leftover.

**And the label declutter learned that two questions are one question.** The
range numerals were tested against a 93 px callsign-sized box when a numeral is
24 px wide, which with three of them reserved instead of two started silently
eating real callsigns. Correcting it immediately produced a label flipping
straight into its neighbour, because `blipLabelSide` chose the side from the
disc edge and the numerals alone and knew nothing about what had already been
placed. Whoever decides *whether* a label appears has to decide *where*: the
declutter pass now tries both sides against real rectangles and carries its
answer forward. Closer targets win contested space.

## v7.2.5 — the second audit, built

Two reviewers went over thirteen panel states and the web console again, with
the previous round's 38 fixes ruled out in advance. Thirty-one of their
findings are implemented here. Two were withdrawn and the reason is the most
useful thing in this release.

**A premise I invented, and what it cost.** The brief handed to both reviewers
said the panel is *"read from 1–3 metres, ambient-glanced… nobody leans in."*
Nobody had measured that. It is a desk display, read from about 0.9 m, and the
largest finding of the audit — that `font_micro13` at 2.9′ sits below the
human acuity limit and most of the panel is therefore unresolvable — was an
artefact of the invented distance. At 0.9 m it is 6.5′: small, but legible.
Six source comments had already absorbed the wrong number as fact. `CLAUDE.md`
now carries a measured *"How it is actually read"* table so the next person
inherits a number rather than an assumption, and the useful inversion is that
at desk distance the display has headroom for **more** information, not less.

**The panel stops asserting things that are not true.** Above forty aircraft
the track table kept the first forty *in the feeder's JSON order* — so the hero
count pinned silently and `NEAREST` could name the wrong aircraft, because the
genuinely closest one was discarded if it appeared forty-first. It keeps the
nearest forty now and says `OF 57` when the sky holds more. A pinned aircraft
that leaves the ring stops showing a cyan `LIVE` dot beside an empty disc. The
web console's `IN RANGE 13` no longer sits beside `HEARD 9`, two numbers that
could not both be true. And coasting callsigns, which measured **2.04:1**
against the map, are marked by a leading `~` at full contrast instead —
because the state where the panel shows a dead-reckoned guess is exactly when
you need to read which aircraft it is.

**It says whether anything is coming toward you.** `NEAREST 23.0 km SE` is a
scalar; it cannot tell an aircraft overhead in four minutes from one that left
twenty minutes ago. Three cosines over data already in `Track` turn it into
`9 KM IN 3 MIN`, or `OUTBOUND`. No network, no PSRAM, no flash, no new glyphs.

**And it remembers an hour.** A 60-byte ring and a 1-bit canvas, repainted
once a minute — three orders of magnitude below the repaint that caused the
v7.2 glitch, and nothing written to flash. It shares the emergency strip's
slot, so it appears only when nothing is wrong.

Also: tap-to-wake during quiet hours used to last under a second (the wake had
no timer, so the next 1 s tick blanked the panel again unless a finger was
still on the glass), and a newly seen emergency squawk now lights the panel for
a minute at 03:00. Labels dodge other aircraft rather than being drawn through
them. The emergency strip names the squawk — `7600 RADIO` — instead of leaving
the reader to translate it. The CARTO attribution, defined since the map
landed and never once drawn, is now in the gutter below the disc. "Panel
password" was renamed **Web & API password**, because it never locked the
panel. The console works on a phone, its destructive button went from 2.87:1 to
6.99:1, and clicking a row in its traffic table now selects that aircraft on
the panel.

Four defects in this release were introduced by the work in it and caught
before shipping: a disclosure that compared two counts measuring different
populations, a coordinate guard that validated only the first character (so
`46,45` still moved the radar), a wind icon with two owners fighting over its
visibility, and a sparkline hidden by any single coasting aircraft. Three of
the four were found only on the hardware.

## v7.2.6 — the console stops growing

Two small things in the web console, both of the kind that only show up once
someone else looks at the page.

**The traffic list was the only element on the page sized by the sky.** Forty
aircraft made its card 1,055 px tall next to a 360 px panel mirror, so the page
ran on for 700 px of empty column and everything below the fold moved position
depending on how busy the afternoon happened to be. It is now bounded by the
height of the card beside it and scrolls inside that, with its header pinned.

The mechanism is worth recording, because the obvious fix does not hold: a
`max-height` in pixels is a guess that drifts with the window, and the card it
is matching is sized by an image whose height depends on the column width. The
row is stretched instead and the scroller is *absolutely positioned* inside the
traffic card — an absolutely positioned child contributes nothing to its
parent's intrinsic height, so the row is sized by the mirror alone and the list
takes whatever that turns out to be. No JavaScript and no measured constant, so
it re-matches on a resize by itself. Below 901 px the rule is off: a phone has
no second column to match, and a vertical scroll nested inside a page scroll is
a trap.

**Every page load asked for `/favicon.ico` and got a 404**, which put a red
error in the console of anyone using this page to debug their own network — a
false lead on a page whose entire job is diagnosis. The fix is to declare an
icon so the request is never made, rather than add a route to answer it: a
route would cost a handler and a payload on every tab. It is an inline SVG data
URI, three circles and a dot, 363 bytes, no flash and no second request.
