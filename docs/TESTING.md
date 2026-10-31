# Testing and review

How AirRadar is tested, reviewed and released, what exists today, and the
plan to get from here to there.

The methodology is adapted from the one used for a Go web application the
same owner maintains. The principles carry over unchanged. Only the tools
change, because this is ESP32 firmware with a desktop harness and a static
web installer, not a server:

- **Nothing ships on a single author's word.** The builder never reviews its
  own work, and the owner approves every merge, tag, release and flash.
- **Every rule is enforced by a check where a check is possible.** A rule
  that matters gets a test, so breaking it fails CI instead of relying on
  someone remembering.
- **Test first.** A bug fix starts with a test that reproduces the bug.

[`CLAUDE.md`](../CLAUDE.md) carries the short, binding version of these
rules ("Testing and review (required)"). This file holds the reasoning, the
concrete gates and the adoption plan.

**Scope.** The v7 firmware under `firmware/AirRadar/`, the UI harness in
`desktop/`, the web flasher in `flasher/`, the release pipeline and the
documentation. The legacy v6 sketch at the repo root (`AirRadar.ino`,
`LGFX_Waveshare_7.h`) is a reference copy, so it gets no new work. It is
excluded from tests, coverage and compile gates for that reason. It stays
inside the secret scan, the repo-policy test and the licence checks, because
it is published like everything else.

---

## 1. Current state (2026-10-31, v7.2.6)

This is an honest snapshot. Most of the target gates do not exist yet.

| Area | Today | Gap |
|---|---|---|
| CI | **None.** There is no `.github/` directory and no workflow. | Every gate below. |
| Unit tests | **None.** Pure logic (geometry, units, filters, parsers, validators) is spread through modules that include `Arduino.h`, `WiFi.h`, FreeRTOS or LVGL. Many of the most important functions are `static` inside `svc/web.cpp`, `net/feeder.cpp` and `svc/mqtt.cpp`. | A logic layer that compiles on a host, plus tests for it (§3). |
| UI harness | `desktop/` compiles the real `ui_*.cpp`, `theme.cpp`, `brandcolor.cpp`, `bezel.cpp`, `spark.cpp`, `state.cpp`, `tracks.cpp` and `units.cpp` against SDL2 and thin shims. It renders 8 scenarios and the settings, legend and time-zone screens, either in a window or headless (`--shot`), and is used for layout review and README images. | Output is **not deterministic**: `millis()` is a real steady clock and `time()` is the real wall clock, so the clock card and any in-flight blip glide differ between runs. There are no baselines and no comparison. `make shots` loops over indices 0–6 and **skips scenario 8** ("pinned target left the ring"). |
| Map preview | `firmware/tools/tintpreview.py` runs `maptiles.cpp`'s tint arithmetic over real CARTO tiles. It's a manual tool. | It isn't wired into anything, which is fine: it needs the network. |
| Target build | It's built on the owner's Mac with `arduino-cli` and exact pins: core 3.3.10, LVGL 8.3.11, LovyanGFX 1.2.25, ArduinoJson 6.21.6 and PubSubClient 2.8.0. | No CI compile. The pins are written out in three places (`firmware/BUILD.md`, `README.md`, `THIRD-PARTY-NOTICES.md`) and nothing checks that they agree. |
| Size | v7.2.6's app image is 1,657,552 bytes, about 53% of the 3 MB app slot (`app3M_fat9M_16MB`). | No budget, so nothing notices growth. Static internal DRAM, the resource that actually runs out (rules 9 and 11), isn't tracked at all. |
| Static analysis | None. | cppcheck and clang-tidy with a baseline. |
| Hardware verification | This is thorough but manual. It reads `GET /screen.bmp`, `/metrics`, `/api/stalls` and `/api/heapwalk` on the owner's panel. Soaks have been measured, for example the 21-hour heap soak in v7.2. | It isn't recorded per release in a fixed form. |
| Reviews | Two independent **design** audits ran: v7.2.4 had 40 findings and v7.2.5 had 31. | No independent **security** review. The security fixes so far (rules 14, 15, 30) were reactive: each was found after it caused a problem. |
| Releases | The binaries are built locally, committed to `flasher/` on `main`, and also attached to the GitHub release. For v7.2.6 the release assets and the committed files have identical SHA-256. | No published checksums, no signature, no provenance. The merged-image check (rule 28) is a hand-run Python snippet in `firmware/BUILD.md` §C. Release assets don't include the licence notices, which the BSD-2 clause in LovyanGFX requires for binary distribution. |
| Web flasher | It's served by GitHub Pages from the **root of `main`**, so merging to `main` publishes the installer immediately. | It loads `esp-web-tools@10` from unpkg: a major-version range, with no SRI and no CSP. Whatever that URL resolves to runs in the page that holds Web Serial access to the user's board. |
| Credentials | Nothing is compiled in. Wi-Fi, the Web & API password and the MQTT URI (which can carry a user and password) are entered at runtime and stored in NVS. `config.h` ships neutral defaults (0,0, `UTC0`, `adsb.local`). No secrets file exists. | NVS isn't encrypted, so anyone with USB access can read the credentials. The Web & API password is optional and defaults to empty (see §7.2). |
| Repo hygiene | No private IPv4 addresses or precise coordinate pairs were found in tracked text. The internal review document and the mockups are gitignored on purpose. | The six generated `font_*.c` files embed an **absolute host path, including a home directory**, in their `Opts:` comment. Nothing checks for any of this. |
| Licences | `LICENSES/` holds a text for each bundled component. Fonts carry `SPDX-License-Identifier: OFL-1.1`. | `THIRD-PARTY-NOTICES.md` says the font files live under `src/ui/`; they're in `src/assets/`. `firmware/lv_conf.h`, `firmware/tools/genassets.py` and `flasher/index.html` have no SPDX header. Nothing checks any of this. |
| Secret scan | None. | gitleaks over the full history, then on every push. |

---

## 2. Principles, adapted

### 2.1 Test-first, always

- Write the test, run it and watch it fail (RED), make the smallest change that
  passes (GREEN), then refactor.
- **Every bug fix starts with a reproduction.** Pick the cheapest layer that
  can reproduce it:
  1. a host unit test (logic, parsing, validation, arithmetic);
  2. a harness baseline or harness assertion (layout, state, invalidation);
  3. a policy or image test (repo content, the merged image, versions);
  4. if only the panel can show it (DMA, shake, touch, heap over hours), a
     **hardware-in-the-loop item** (§6), with the `/api/stalls` or `/metrics`
     reading that distinguishes broken from fixed, written down *before* the
     fix. This follows rule 27: a measurement only counts if the correct value
     is known in advance.
- Coverage: **at least 80% per file of the logic layer**, plus a total for the
  layer. Exclusions are listed and justified in §4.3.

### 2.2 Flaky tests are bugs

- The harness is the obvious source: the clock card reads the wall clock, the
  blip glide is driven by real `millis()`, and `--shot` waits with
  `SDL_Delay`. The fix is a frozen clock (§4.7), never a looser threshold or a
  retry.
- A fuzz run that finds something once and not again is still a finding. Keep
  the crashing input in the corpus.
- On hardware, something that works "most power cycles" is the GT911 address
  coin flip of rule 4: a product bug, not test noise.

### 2.3 What each layer can and cannot prove

| Layer | Faithful about | Blind to |
|---|---|---|
| Host unit tests | Arithmetic, parsing, validation, state machines, string handling | Anything 32-bit, Xtensa, FreeRTOS or timing, unless built for it (§4.2) |
| UI harness | Layout, typography, which objects exist, **how much LVGL invalidates** | DMA, MSPI contention, panel colour, touch, the real map |
| Target compile | That it builds with the pinned toolchain, warnings, section sizes | Behaviour |
| Flasher browser tests | Page, manifest, image layout, accessibility, CSP | Whether a real board boots the image |
| HIL checklist | Everything physical | Nothing physical, but it's manual and slow, so it's run per release |

This is the same split `desktop/README.md` describes, made into a policy:
**never accept a harness result as evidence about the panel, and never send
a layout question to the panel** when the harness can answer it in two
seconds.

---

## 3. Host-side unit tests

### 3.1 Extract a logic layer first

The tests need code that compiles without the platform. Today the testable
logic is mixed with I/O, so the first engineering step is extraction. It's a
refactor, so behaviour must not change, and every extracted function gets its
tests in the same change.

Rules for the layer (proposed home: `firmware/AirRadar/src/logic/`):

- No `Arduino.h`, `WiFi.h`, FreeRTOS, LVGL or LovyanGFX includes. ArduinoJson
  is allowed: it's header-only, it runs on the host, and testing the parsers
  against the **same pinned 6.21.6** is the point.
- **Plain C types in and out** (`const char*` plus a length, fixed buffers,
  ints, floats). Callers keep their `String` adapters. This matters because
  of rule 30: the `46,45` bug was caused by Arduino `String::toDouble()`
  semantics. A test written against the harness shim's `String` would be
  testing the shim, not the firmware. Where logic must use `String`, add a
  shim-fidelity test pinning the shim's behaviour to Arduino's for every
  method that logic relies on.
- No globals read inside the logic. State (`g_set`, `g_tracks`, the local IP
  for `hostAllowed`) is passed in. That is also what makes it fuzzable.
- The harness and the firmware compile the same files. Nothing is copied.

Candidates, in rough priority order (security and data integrity first):

| Logic | Lives in today | Why it is first |
|---|---|---|
| `numericArg`, `rangeStepValid`, `argBool`, hex validation in `handleApiSelect`, `apiCfgValidate` | `svc/web.cpp` (static) | User input. Rule 30 damaged live stored data. |
| `hostAllowed` and the Origin comparison in `authed()` | `svc/web.cpp` (static) | Rule 15, twice. The only CSRF and DNS-rebinding defence. |
| `htmlEscape`, `mqttUriRedacted` | `svc/web.cpp` (static) | XSS in the console; credential leak. |
| `mqttParseUri`, `mqttBuildClientId` | `svc/mqtt.cpp` (static) | Parses a user string that carries credentials. |
| `parsePlane`, `buildAltUrl`, `urlAuthority`, the dialect/filter selection in `fetchParse` | `net/feeder.cpp` (static) | Untrusted network JSON over plain HTTP on the LAN. |
| Callsign sanitising, `trimCopy`, route, weather and ISS parsing, the "only 200/404 sets `routeTried`" rule | `net/enrich.cpp` | Untrusted network JSON; ISS is plain HTTP on purpose, so it is MITM-able. |
| `logosIcaoFromFlight`, logo cache key/path | `net/logos.cpp` | A wire value becomes a file path. |
| `chooseZoom`, cache key (`AR_MAP_RECIPE`, rule 24), tint and lens arithmetic | `net/maptiles.cpp` | Rule 24 is a silent-forever failure. |
| `haversineKm`, `bearingTo`, `scopeToScreen`, `trackClass`, `trackPassesFilters`, `trackOnWatchlist`, `altColorRGB` | `core/state.cpp` | Already host-compiled by the harness, so they are cheap to test. |
| Track merge, dead reckoning, ordering, coasting count, lifecycle thresholds | `core/tracks.cpp` | The fresh/coasting/dropped state machine. |
| `tlsTryAcquire`/`tlsGateOpen` decision (heap free and largest block in, verdict out) | `core/state.cpp` | Rule 11. Pure once the heap figures are parameters. |
| Unit conversion, night-mode window (wraps midnight), range-commit debounce | `core/units.cpp`, `core/state.cpp`, UI | Display-only units contract; time windows wrap. |
| `brandRgbFor`, `fnv1a`, `hueToRgb`, sparkline ring buffer | `ui/brandcolor.cpp`, `ui/spark.cpp` | Cheap. Already host-compiled. |
| Time-zone table invariants | `core/timezones.h` | Every name must be inside `font_body18`'s subset (0x20–0x7E, 0xB0, 0xB7). An em dash renders as a box on the panel and looks fine everywhere else. |

### 3.2 Runner: doctest, built by a host Makefile

**Recommendation: [doctest](https://github.com/doctest/doctest)** (MIT,
single header, vendored with its licence), built by a `tests/host/Makefile`
that reuses `desktop/shim/` and takes ArduinoJson from the same Arduino
library folder the firmware builds with. This is how the harness already
reuses LVGL.

Alternatives considered:

- **PlatformIO `native` + Unity.** This is the conventional embedded answer,
  and it is rejected here. It brings a second build system and a **second set
  of library pins** in `platformio.ini`, which would drift from
  `arduino-cli`'s, and the project's stated property is that the firmware is
  one `arduino-cli compile` away. The tests would run against PlatformIO's
  view of the libraries, not the ones the release is built from.
- **GoogleTest.** Capable, but it wants CMake or a prebuilt library, which
  the harness deliberately avoided ("no cmake, no second copy of LVGL"), and
  it adds nothing doctest lacks at this size.

doctest also composes with the rest of the plan: the same objects link into
libFuzzer targets (§4.8) and build under the sanitizers and coverage flags.

### 3.3 Regression tests owed to `CLAUDE.md`

Each non-negotiable rule was a real debugging session. Where a rule can be a
test, it becomes one, so the lesson can't be lost.

| Rule | Test |
|---|---|
| 14: a blank secret never overwrites | Extract the save decision (`apply(old, submitted) -> new`). Test that blank `pass`, `ppass` and MQTT password keep the stored values, and that a non-blank one replaces them. |
| 15: Host allowlist, then exact Origin | A table of Host/Origin pairs: `airradar.local.evil.com`, rebinding (`Host: evil.com` + `Origin: http://evil.com`), port suffixes, case, absent headers, the device IP. |
| 17: `vTaskDelete(NULL)` skips destructors | A policy check: in a function that calls `vTaskDelete(NULL)`, the enclosing scope must not declare a non-trivially-destructible local (start with `DynamicJsonDocument`, `String`, `HTTPClient`). A grep-level script first; clang-tidy later. |
| 18 and 29: counters and comparable counts | Every metric published on `/metrics` must be written somewhere (a policy check over `web.cpp` and the metric sources), and the `OF n` disclosure test compares the same population. |
| 20: `LV_INV_BUF_SIZE` overflow | Harness: the crowded scenario must never produce a full-screen flush after the first frame (§4.7). |
| 8 and 19: change-cache every write; z-order invalidates the parent | Harness: a static scenario must flush **zero** pixels on later ticks (§4.7). |
| 24: the cache key covers the recipe | Changing `AR_MAP_RECIPE` must change the key, and so must a style or tint constant (assert they all feed the key). |
| 26: the content box subtracts the border | A harness assertion: `lv_obj_get_content_width()` of each card equals `CONTENT_W` in `ui_cards.cpp`. Comparing the constant with its own formula would prove nothing (rule 27); comparing it with what LVGL actually lays out does. |
| 28: the merged image uses `--flash-mode keep` | Image test: byte 2 of the merged image is `0x02`, and each component is byte-identical to the build output (§4.9). |
| 30: validate the whole string | `numericArg` with `46,45`, `1e5`, `--1`, `.`, `1.2.3`, whitespace, an empty string, and very long input. |
| Time-zone and font subsets | Every `kTimezones[].name` is inside `font_body18`'s ranges, and every `font_*.c` covers the codepoint range `CLAUDE.md` documents for it (parse its `Opts:` line). |

---

## 4. CI gates (target)

Every gate blocks on `main` once it exists. Until a gate exists, its check is
done by hand and the result is stated in the PR or merge message.

### 4.1 Lint

- **actionlint** for workflows and **shellcheck** for any shell script.
- **ruff** for `firmware/tools/*.py` and any Python CI helper.
- **clang-format** only for new files under `logic/` and `tests/`.
  Reformatting the existing ~10,600 lines would bury every real diff in
  `git blame`, so the rest of the tree stays as it is.

### 4.2 Host unit tests with sanitizers

- Build the tests with `-std=gnu++17 -Wall -Wextra -Werror` for first-party
  code only.
- Run the tests under **ASan + UBSan** (`-fsanitize=address,undefined
  -fno-sanitize-recover=all`), so undefined behaviour fails the run instead of
  printing a warning.
- **Also run a 32-bit build** (`-m32` on the Linux runner). The target is a
  32-bit Xtensa where `long`, `size_t` and pointers are 4 bytes, and host-only
  64-bit tests hide width bugs. If `-m32` proves impractical, the fallback is
  `static_assert`s on every width assumption in the logic layer.
- The harness itself is also built and run once under ASan + UBSan in CI. It
  executes the real UI code against LVGL, so it catches out-of-bounds access
  in `ui_*.cpp` that no unit test reaches.

### 4.3 Coverage

- Use clang source-based coverage (`-fprofile-instr-generate
  -fcoverage-mapping`, `llvm-cov report`). Clang is needed for libFuzzer
  anyway.
- **At least 80% line coverage per file of `logic/`**, plus a total for the
  layer. The threshold lives in a checked-in file, and lowering it is a
  reviewed change with a stated reason.
- Excluded, with reasons:
  - `hal/` (hardware only; covered by HIL);
  - `ui/ui_*.cpp` (covered by visual baselines, where line coverage would
    measure the wrong thing);
  - `svc/heapwalk.cpp` (an ESP-IDF heap walker with no host equivalent);
  - `src/assets/` (generated data);
  - network task bodies (I/O shells; their decisions move into `logic/`).

### 4.4 Target compile

Use `arduino-cli` on a hosted Ubuntu runner, so CI builds exactly what the
owner builds:

1. Install `esp32:esp32@3.3.10` and the four libraries at their exact pins,
   read from **one pin file** (proposed `ci/pins.env`). The docs-sync test in
   §4.11 checks `BUILD.md`, `README.md` and `THIRD-PARTY-NOTICES.md` against
   it.
2. Copy `firmware/lv_conf.h` beside the lvgl folder (the step everyone gets
   wrong, so CI does it on every run).
3. Compile with the FQBN from `BUILD.md`, `--warnings all` and
   `--export-binaries`.
4. Cache `~/.arduino15` and the library folder, keyed on the pin file.

**Warnings as errors, where feasible.** `-Werror` through `arduino-cli` also
applies to the core and the libraries, whose warnings aren't ours to fix. So
CI filters the compiler output down to warnings in `firmware/AirRadar/` and
fails on any warning that isn't in a checked-in baseline. The baseline starts
at whatever exists today and only ever shrinks. The host build (§4.2) is
already `-Werror` for the same first-party files.

### 4.5 Flash and RAM budgets

Read the ELF with `xtensa-esp32s3-elf-size -A` (shipped with the core) and
the app `.bin` size. A checked-in `ci/size-budget.json` holds a limit for:

| Metric | Why |
|---|---|
| App image bytes vs the 3 MB OTA slot | An image that doesn't fit can't be installed over the air. |
| Flash text + rodata | Growth trend. |
| **Static internal DRAM** (`.dram0.data` + `.dram0.bss`) | Internal SRAM is the scarce resource. Every static byte comes out of the heap that `AR_TLS_HEAP_FLOOR` (45 KB) and `AR_TLS_BLOCK_FLOOR` (20 KB) protect (rules 9 and 11). |
| IRAM | Tight on the S3; overflow is a link failure, so catch it early. |

The rules:

- CI fails when any metric exceeds its budget.
- The budget is set at today's value plus a stated headroom.
- Raising a budget is a reviewed change with a reason, for example "LVGL
  feature X, +6 KB DRAM, measured heap_min on the panel afterwards: …".
- CI posts the delta against `main` on every PR.

Runtime heap (free, minimum, largest block) can't be measured in CI. It's an
HIL item (§6).

### 4.6 Static analysis

- **cppcheck** over `firmware/AirRadar/src/` (excluding `assets/`) with
  `--enable=warning,performance,portability`. It doesn't need a compile
  database, so it also covers `net/`, `svc/` and `hal/`, which don't build on
  a host.
- **clang-tidy** over everything the host Makefiles compile (logic, core,
  UI), using a generated `compile_commands.json`. Start with
  `bugprone-*`, `clang-analyzer-*`, `cert-*` (selected) and `performance-*`.
- **Baseline:** today's findings go into a checked-in suppression file, each
  with a one-line reason. New findings fail CI. The baseline is worked down
  over time and never grows without review.

### 4.7 Visual baselines and repaint budgets (the harness)

This is the embedded version of "visual regression in a pinned container".
The harness already renders the real UI, so it only has to be made
deterministic and compared.

1. **Freeze time.** Add a harness flag (for example `--epoch <unix>`) that
   fixes `time()` and drives `millis()` from a counter advanced by the
   render loop instead of the wall clock. The `--shot` path then steps a
   fixed number of ticks instead of sleeping. The scenarios already compute
   ages relative to `millis()`, so they keep working.
2. **Fix `make shots`** to cover all `SCN_COUNT` scenarios (it currently
   skips the eighth). Add the settings screen at several `--scroll` offsets,
   the legend, the time-zone picker, and `--imperial` for the screens that
   show units.
3. **Render in a pinned container image** (the compiler, SDL2 and the LVGL
   checkout at 8.3.11, referenced by digest). LVGL rasterises in software
   from the committed font data, so output should be bit-exact. The
   comparison threshold therefore starts at **zero differing pixels** and is
   loosened only if a measured, explained source of variation appears.
4. **Baselines are PNGs** converted from the BMPs, stored under
   `desktop/baselines/`. They are regenerated **only** through an explicit
   `workflow_dispatch` input, and are reviewed and leak-checked (§7.3) before
   they're committed. A failing comparison uploads expected, actual and diff
   images as artefacts.
5. **Repaint budgets.** These test what the harness measures faithfully:
   LVGL's own invalidation. Count flushed pixels per tick in the harness's
   `flush_cb`:
   - a scenario with nothing changing must flush **0 px** on every tick after
     it settles (rules 8 and 19: unchanged writes and z-order changes
     repaint);
   - the crowded scenario must never flush the full 800×480 after the first
     frame (rule 20);
   - an ordinary update of the typical scenario stays under a stated pixel
     budget.

   These would have caught the "wiggle" and the 52%-of-screen repaint
   without a panel.

The harness ground is synthetic, so **no map judgement is ever made from a
baseline**. That stays with `tintpreview.py` and the panel.

### 4.8 Fuzzing

Use libFuzzer on the host build with ASan + UBSan, one target per parser of
untrusted input:

| Target | Input source | Trust |
|---|---|---|
| Feeder `aircraft.json` (both dialects) through the real filter and `parsePlane` | LAN feeder (plain HTTP), airplanes.live | Untrusted |
| adsbdb route JSON, Open-Meteo JSON, open-notify JSON | Internet; ISS is plain HTTP | Untrusted |
| `mqttParseUri` | Console form, `/api/config` | User |
| `/api/config` POST body through `apiCfgValidate` | LAN client | Untrusted |
| Form validators: `numericArg`, hex, range, Host and Origin pair | LAN client | Untrusted |
| `buildAltUrl`, `urlAuthority`, callsign sanitising, logo key/path | Settings and wire data | Mixed |
| PNG decode of tiles and logos (LovyanGFX's decoder, if it builds on the host) | CARTO and logo hosts over TLS | Untrusted (later phase) |

Run settings:

- **PR:** each target runs 60 s from the committed corpus, with a fixed seed
  so the run can be reproduced.
- **Nightly:** 30 minutes per target. New crashes open an issue with the
  minimised input.

**Corpus inputs are synthetic.** A real `aircraft.json` from the owner's
feeder is a map of the sky around the owner's home: positions cluster on the
receiver. It is personal data, and it never enters the repo. Write seeds by
hand, or generate them around the harness's synthetic location.

### 4.9 Web flasher

The flasher is a public page on GitHub Pages with Web Serial access to the
visitor's board. That makes it the project's most exposed artefact.

**Supply chain first.**

1. **Vendor ESP Web Tools.** Pin an exact version at least 14 days old,
   verify the npm integrity hash, copy the built bundle into
   `flasher/vendor/`, add its Apache-2.0 licence to `LICENSES/` and
   `THIRD-PARTY-NOTICES.md`, and let Renovate propose updates. SRI on the
   unpkg URL isn't enough: `install-button.js` loads further chunks by
   dynamic `import()`, and SRI covers only the entry script.
2. **CSP in a `<meta>` tag**, since Pages cannot set headers:
   `default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline';
   img-src 'self' data:; connect-src 'self'; base-uri 'none'; form-action
   'none'`. Move the inline `<style>` to a file or hash it, and drop
   `'unsafe-inline'` if ESP Web Tools allows it. `frame-ancestors` and
   report-only mode don't work from a meta tag. That's why the Playwright
   test below listens for violations.

**Tests.** Use Playwright with a lockfile under `tests/web/`, against
`flasher/` served by a local static server:

- **Smoke:**
  - the page loads with no console errors and no failed requests;
  - after vendoring, **no request leaves the origin**;
  - `customElements.get('esp-web-install-button')` is defined;
  - the manifest fetch returns 200;
  - in Firefox and WebKit the `unsupported` slot is shown (no Web Serial
    there), which tests the fallback text users actually see.
- **CSP:** zero `securitypolicyviolation` events.
- **Accessibility:** an axe scan (WCAG 2.2 A/AA) at desktop width and at 390,
  360 and 320 px, with no horizontal scroll at those widths. The button is
  reachable by keyboard and its focus is visible. The page has one dark
  theme, so axe runs on that theme.
- **Optional:** an OWASP ZAP passive baseline against the served page. It's
  cheap, and it checks the CSP for mistakes.

**Manifest and image test.** A small script, also run in the release
workflow, checks:

- `manifest.json` parses, `chipFamily` is `ESP32-S3`, the single part is at
  offset 0, every `path` exists, and `new_install_prompt_erase` is `true`
  (the merged image wipes NVS, so the prompt must stay);
- the version agrees in four places: `AR_VERSION` in `config.h`,
  `manifest.json`, the `index.html` footer and the release-notes box, and
  the tag;
- the merged image:
  - byte 0 is `0xE9`;
  - **byte 2 is `0x02` (DIO)** (rule 28);
  - the partition table at `0x8000` matches `app3M_fat9M_16MB`;
  - `boot_app0` is at `0xe000`;
  - the app at `0x10000` is **byte-identical** to `airradar-ota.bin`;
  - in CI, every component is byte-identical to the build output;
- `airradar-ota.bin` contains the `AR_VERSION` string;
- once releases are CI-built, both `.bin` files match the release's
  `SHA256SUMS` (§5).

### 4.10 Secret scan

- Run **gitleaks** once over the full history (137 commits today). Each hit is
  either rotated and recorded, or allowlisted with a reason.
- After that, run it on every push and PR.
- Custom rules, on top of the defaults:
  - `mqtt://user:pass@` with a non-placeholder password;
  - `Authorization: Basic` values;
  - `ssid`/`pass`/`ppass` string literals that aren't empty.

### 4.11 Repo-policy tests

A checked-in script (`ci/policy.py`; Python is already the project's tooling
language) fails CI on any of the following. It prints file:line, never the
matched text.

- **Private and personal identifiers:**
  - private IPv4 ranges anywhere in tracked text (documentation ranges
    192.0.2.0/24, 198.51.100.0/24 and 203.0.113.0/24 are allowed);
  - MAC addresses;
  - absolute host paths (`/Users/`, `/home/`, `C:\Users\`). This fails
    today, on the six `font_*.c` headers: regenerate them with relative
    `-o` paths, or have `genassets.py` normalise the comment;
  - coordinate pairs with three or more decimals outside an allowlist (the
    harness's round synthetic home is fine);
  - real hostnames other than the public services the firmware talks to.
- **A private identifier list kept outside the repo.** The owner's real
  location, network names, hostnames and addresses are supplied to CI as a
  secret, and as a local file outside the working tree for local runs.
  That's what lets the check exist without itself publishing what it looks
  for.
- **Neutral defaults stay neutral:**
  - `AR_DEFAULT_LAT`/`LON` are `0.0`;
  - `AR_DEFAULT_TZ` is `UTC0`;
  - `AR_DEFAULT_FEED` points at `adsb.local`;
  - the harness sets only its synthetic location.
- **Credentials never exist at build time.** No tracked file may define a
  Wi-Fi SSID or password, MQTT credentials or a Web & API password. If a
  bring-up convenience header is ever introduced (for example `secrets.h`),
  it must be gitignored, and the test fails if it is tracked.
- **Internal documents stay internal.** `UI_UX_REVIEW.md`, `docs/mockups/`
  and the superseded plans are never tracked (the `.gitignore` entries
  remain, and the test asserts it).
- **Images:** files under `docs/img/` carry no EXIF or other metadata (GPS
  in particular). What they show is the leak hunt's job (§7.3).
- **Docs match the build:**
  - the library and core pins in `BUILD.md`, `README.md` and
    `THIRD-PARTY-NOTICES.md` equal `ci/pins.env`;
  - the FQBN in `README.md` equals `BUILD.md`'s;
  - the font codepoint ranges in `CLAUDE.md` match the `Opts:` line of each
    `font_*.c`;
  - the verify doc matches the release workflow (§5).
- **Flasher hygiene:** no `src=` or `href=` to a third-party origin in
  `flasher/*.html` once ESP Web Tools is vendored.

### 4.12 Licences and notices

- Every component listed in `THIRD-PARTY-NOTICES.md` has its text in
  `LICENSES/`, and every file in `LICENSES/` is referenced.
- Paths in the notices exist. This would catch today's `src/ui/` vs
  `src/assets/` drift.
- Every first-party source carries an SPDX header:
  - `GPL-3.0-or-later` for first-party code;
  - `OFL-1.1` for the generated fonts;
  - `lv_conf.h` is derived from LVGL's MIT template, so it takes an MIT
    notice, not GPL.
- **Release assets include the notices** (`LICENSES/` and
  `THIRD-PARTY-NOTICES.md` as an archive). LovyanGFX's BSD-2 clause 2 requires
  the notice to travel with binary distributions, and today the binaries
  travel without it. The flasher page links to the notices.
- If a new bundled component appears (a vendored library, a font, ESP Web
  Tools), the test fails until its licence is added.

### 4.13 Dependencies and supply chain

- **Actions are pinned to a full commit SHA**, with the tag in a comment.
  Permissions are minimal (`contents: read` by default). Workflow outputs go
  through `env:`, never inlined into `run:`.
- **A 14-day minimum age** for any new core, library, tool, action or npm
  package, plus a checksum or signature check when it is downloaded in CI.
- **Renovate** for Actions, npm (`tests/web/`, the vendored flasher bundle)
  and pip. A regex manager over `ci/pins.env` proposes core and library bumps.
  `README.md` says each pin is load-bearing, so these PRs are never
  auto-merged; each one needs the full gate set plus an HIL pass.
- **An advisory watch** (weekly scheduled job). The Arduino libraries have no
  lockfile that osv-scanner understands, so the job checks the pinned
  versions of arduino-esp32 (and the ESP-IDF/mbedTLS it ships), LVGL,
  LovyanGFX, ArduinoJson and PubSubClient against GitHub security advisories
  and OSV, and opens an issue on a match. `osv-scanner` covers the npm and pip
  lockfiles.

### 4.14 Gate summary

| Gate | Tool | Runs on | Phase |
|---|---|---|---|
| Lint | actionlint, shellcheck, ruff | PR, push | 1 |
| Secret scan | gitleaks | PR, push (full history once) | 1 |
| Repo policy, docs sync, version consistency | `ci/policy.py` | PR, push | 1 |
| Licences and notices | `ci/policy.py` | PR, push | 1 |
| Target compile and warnings baseline | arduino-cli | PR, push | 1 |
| Size budgets | `xtensa-esp32s3-elf-size` | PR, push | 1 |
| Merged-image and manifest test | script | PR, push, release | 1 |
| Harness build and headless smoke | make, SDL2 | PR, push | 1 |
| Unit tests (64- and 32-bit), ASan + UBSan | doctest | PR, push | 2 |
| Coverage ≥ 80% per logic file | llvm-cov | PR, push | 2 |
| Visual baselines, repaint budgets | harness in a pinned container | PR, push | 3 |
| Static analysis with a baseline | cppcheck, clang-tidy | PR, push | 4 |
| Flasher: Playwright, axe, CSP | Playwright | PR, push | 4 |
| Fuzzing | libFuzzer | PR (60 s), nightly | 5 |
| Release: checksums, cosign, provenance, SBOM | release workflow | tag | 5 |
| Advisory watch | scheduled job | weekly | 5 |

---

## 5. Releases

**Today:** built on the owner's Mac, committed to `flasher/`, attached to the
GitHub release, served by Pages from `main`.

**Target:** CI builds the release, and nothing built on a laptop is
published.

1. Tag `vX.Y.Z` on a commit of `main` whose CI is green (checked by full
   SHA).
2. On a **GitHub-hosted** runner, the release workflow:
   - rebuilds with the pinned toolchain;
   - produces `airradar-ota.bin` and the merged `airradar-merged.bin`
     (with `--flash-mode keep --flash-freq keep`, rule 28);
   - runs the manifest and image test (§4.9);
   - writes `SHA256SUMS`.
3. It signs each binary and `SHA256SUMS` with **cosign keyless**
   (`cosign sign-blob --bundle <file>.sigstore.json`).
4. It attests **build provenance** (`actions/attest-build-provenance`) and an
   **SBOM** (`actions/attest-sbom`). The SBOM is generated from
   `ci/pins.env`: the core, the four libraries and the ESP-IDF version the
   core ships. Syft can't see Arduino libraries.
5. It publishes a **draft** release with the binaries, `SHA256SUMS`, the
   bundles and the notices archive. The owner reviews it and publishes it.
6. The flasher copy in `flasher/` is updated by a commit that contains
   **exactly** the release artefacts. The image test checks them against
   `SHA256SUMS`. Merging that commit is what makes the new installer live,
   so it needs the owner's OK like any release.

   An alternative worth considering: switch Pages to an Actions deployment
   that pulls the assets from the release. That takes binaries out of git
   entirely and makes "merge to main" stop meaning "publish the installer".

**A verify doc** (proposed `docs/VERIFY.md`) shows a user how to check a
download: `sha256sum -c SHA256SUMS`, `cosign verify-blob` with
`--certificate-identity` pinned to the release workflow and `--certificate-oidc-issuer
https://token.actions.githubusercontent.com`, and `gh attestation verify
<file> --repo MrRonco/AirRadar`. A policy test keeps its workflow path,
identity and file names in step with the release workflow.

### 5.1 Secure boot, flash encryption and OTA authenticity

- **Secure Boot v2 and flash encryption** are eFuse-based and one-way. On
  this stack they'd need a custom bootloader (an ESP-IDF build rather than
  the prebuilt Arduino core). A public image signed with the owner's key
  would also lock every user's board to that key. They are **not suitable
  for the published build.** They stay a documented option for an owner who
  wants their own panel hardened against physical credential readout. That
  is the real gain: NVS currently holds the Wi-Fi password, MQTT credentials
  and the Web & API password in plain form.
- **OTA authenticity is the more relevant control.** Today `/update`
  accepts any image that passes ESP-IDF's format and digest checks, so the
  only gate is the Web & API password, which is optional. Two options are
  worth a decision by the owner:
  1. Make setting a password part of first boot.
  2. Verify a detached signature in the firmware before `Update.end()`,
     using an embedded public key and the mbedTLS already in the image.

  Option 2 needs a long-lived signing key held by CI. cosign keyless
  signatures can't be checked by the device. It also needs a story for
  self-builders, such as a compile-time key or an explicit "accept
  unsigned" setting.

---

## 6. Hardware-in-the-loop checklist

CI can't see the panel. Each release candidate gets this checklist on real
hardware, and the result is recorded in `docs/hil/vX.Y.Z.md`: date, board
revision, firmware SHA, who ran it, pass or fail per item, and the readings.
Readings are written **without** IP addresses, coordinates or network names,
and the leak hunt (§7.3) covers the record.

**Never install the merged image on the configured panel.** It is a
first-install image: it pads `0x9000–0xdfff` with `0xFF`, which wipes NVS,
including the Wi-Fi password, which is never served back (rule 28). First
install is tested on a **spare or deliberately erased board**.

| # | Item | Pass |
|---|---|---|
| 1 | Merged image via the web flasher, on a spare board | Boots to the Wi-Fi picker. No watchdog loop on the serial console. |
| 2 | First boot: Wi-Fi picker, touch keyboard, numpad location | Connects, map arrives within ~30 s |
| 3 | GT911 address over 5 power cycles | Touch works every time (rule 4) |
| 4 | Display: no black screen, correct colours | PSRAM is OPI (rule 1). The colour order is right. |
| 5 | Touch targets: gear, `?`, both range chevrons, legend dismiss | Each does exactly one thing. The chevrons step in their own direction (the v7.2.6 bug). |
| 6 | OTA from the previous release to the RC on the configured panel | Settings survive. The new version shows in `/metrics`. |
| 7 | OTA with a truncated or corrupt image | Rejected; still boots the old version |
| 8 | With a Web & API password set: unauthenticated `/update`, `POST /api/config` and `/api/heapwalk` | 401 |
| 9 | A request with a foreign `Host` | 403 (rule 15) |
| 10 | 30-minute watch, plus `/api/stalls` | No whole-screen shake. No long stalls with zero flushed pixels beyond the previous release's rate (rule 23). |
| 11 | Heap soak (2 h minimum; **24 h** if the change touches tasks, TLS, JSON documents or flash writes) | `heap_min` and `heap_largest` stay above `AR_TLS_HEAP_FLOOR`/`AR_TLS_BLOCK_FLOOR`; no unexpected shed fetches; no reboot |
| 12 | Map cache across a reboot, and after an `AR_MAP_RECIPE` bump | Cached map reused; recipe change refetches (rule 24) |
| 13 | MQTT to Home Assistant | Discovery entities appear; the emergency binary sensor exists |
| 14 | Night mode | Blanks in the window; a tap wakes it for 30 s |
| 15 | Feeder failover | Local, then the alternate path, then CLOUD, shown correctly |

A **replay feeder** would make several of these repeatable: a small host
tool that serves the harness scenarios as `aircraft.json`, including an
emergency squawk, an extreme-values set and malformed JSON. The panel's
feeder URL is pointed at it for the HIL run. It's a later-phase tool.

---

## 7. Independent review

### 7.1 The flow

1. Work happens on a branch, with one git worktree per parallel task.
2. When the builder says it's done, a **separate security reviewer** reads
   the diff and doesn't edit anything. Each finding gets a rank (CRITICAL,
   HIGH, MEDIUM or LOW), a `file:line`, a failure scenario and a fix.
3. The builder fixes the findings **test-first**: each fix comes with the
   test that would have caught it.
4. The **same reviewer re-checks** the fixes. This repeats until nothing
   CRITICAL, HIGH or MEDIUM is open. LOW findings are fixed or written down
   with a reason.
5. A code review (correctness, the threading contract, the `CLAUDE.md`
   rules) runs alongside the security review. For UI changes, a design
   review uses the harness.

### 7.2 What counts as security-sensitive here

These always get the independent security review, whatever the size of the
change:

- **Network input parsing:**
  - the feeder JSON (plain HTTP on the LAN, and the cloud fallback);
  - adsbdb, Open-Meteo and open-notify (the last is plain HTTP on purpose,
    so it is MITM-able);
  - CARTO tiles and logo PNGs, decoded on the device;
  - MQTT;
  - any buffer, `strlcpy`, `snprintf` or ArduinoJson capacity change in these
    paths.
- **The web console and API:**
  - `authed()`, the Host allowlist and the Origin check (rule 15);
  - form and JSON validation (rule 30);
  - HTML escaping;
  - `/api/probe`, which fetches an arbitrary `http://` URL from the device:
    a LAN request-forgery primitive by design, guarded only by `authed()`;
  - `/api/heapwalk`, which returns heap block **contents**;
  - `/screen.bmp`, which shows whatever the panel shows.
- **Credential storage:** NVS keys `pass`, `ppass` and `mqtturi`, the
  blank-secret rule (rule 14), redaction (`mqttUriRedacted`), and the rule
  that a secret is never served back.
- **OTA and update paths:** `/update`, `Update.*`, partition and boot-slot
  handling, and anything in the release pipeline that produces images.
- **The web flasher supply chain:** ESP Web Tools, the manifest, the
  `flasher/*.bin` files, the Pages configuration and the release workflow.

**Hardware-risk changes** are this project's equivalent of "device control",
and they get review plus an HIL pass:

- anything that writes flash (rule 22);
- anything that changes boot order (the harness README records a probe
  build that boot-looped the panel before OTA came up);
- anything that changes how the merged image is built.

**Where the first security review should start.** These are observations,
not verdicts, because finding the verdict is the reviewer's job:

- The Web & API password defaults to empty, and with it empty `authed()`
  admits any LAN client that passes the Host check. That includes `/update`
  and `/api/heapwalk`. A direct LAN client chooses its own `Host` header.
- `handleUpdateUpload` checks `authed()` only at `UPLOAD_FILE_START`. Later
  chunks rely on `Update.write()` refusing to write when `Update.begin()`
  never ran.
- Credentials sit in NVS in plain form; see §5.1.

### 7.3 The leak hunt

Before anything is published (a release, a README image, a baseline, a
fixture, an HIL record), a separate pass checks for personal data. It covers
all text, fixtures and image metadata, and **what the screenshots actually
show**:

- **A device capture shows the home area.** `/screen.bmp` renders the
  CARTO ground around the owner's location, with the bearing bezel centred
  on it, so a panel screenshot is a map of where the owner lives. Prefer
  harness captures, which are synthetic. If a device capture is needed,
  it's checked for the map, the settings values, the clock's time zone and
  the feeder name.
- **Console screenshots** show IPs, the feeder host, the SSID and
  coordinates. Use documentation placeholders, as `README.md` already
  describes.
- **Feeder captures and logs** reveal the receiver location from aircraft
  positions alone.
- **Metadata:** EXIF/GPS, and absolute paths in generated files (the
  `font_*.c` headers today).

The private identifier list the hunt and the policy test use lives
**outside** the repo (§4.11).

### 7.4 Review checklist for firmware changes

These come straight from `CLAUDE.md`, and a reviewer checks them on every
change:

- **Threading:** LVGL, `g_tracks` and NVS are touched in loop context only.
  Tasks snapshot their inputs, write `g_pending*` under `g_dataMux` and set
  the ready flag last.
- **Tasks:**
  - `tlsGateOpen()` is checked before the spawn (rule 11);
  - no RAII object shares a scope with `vTaskDelete(NULL)` (rule 17);
  - one in-progress flag per task kind.
- **Rendering:**
  - every dynamic LVGL write is change-cached, flags included (rule 8);
  - no `lv_obj_move_foreground()` per item (rule 19);
  - a blip holder bounds its children (rule 12).
- **Flash writes:** frequency, not size, is the cost (rule 22). Any new
  write path states its rate.
- **Instrumentation:** every new counter is incremented somewhere (rule 18).
- **Units** convert only at the point of drawing. Altitude, ground speed and
  vertical rate never convert.
- **Cache keys** cover the recipe (rule 24).

---

## 8. Integration and release flow

1. CI is green on the **branch tip**, checked by its full SHA.
2. Merge with `--no-ff`, with the message written to a file and passed by
   `-F`.
3. CI on `main` is verified by the **full merge commit SHA**, not by "the
   latest run".
4. Tag `vX.Y.Z` (matching `AR_VERSION`), then run the release flow (§5).

**`main` is production.** Pages serves `flasher/` from `main`, so a merge
that changes the flasher page or its binaries is a deploy to every future
visitor. No merge, tag, release or device flash happens without the
**owner's OK**.

**Deploying to the owner's panel** is an OTA upload. It's done by a script,
never by hand:

1. Record the current version and `/metrics` (no secrets are served, by
   design).
2. Upload `airradar-ota.bin`.
3. Wait for the reboot, then verify:
   - the version in `/metrics`;
   - that the feed is live and the track count is plausible;
   - heap above the floors;
   - no new `/api/stalls` signature.
4. If a check fails, OTA the previous release's `airradar-ota.bin` back.
   Before relying on the bootloader's automatic app rollback, confirm the
   prebuilt core enables it.

The owner is told in advance what the deploy will trigger. For example, the
Home Assistant MQTT entities go unavailable during the reboot.

**Soak before the public installer moves.** A release candidate runs
unchanged on the owner's panel before `flasher/` is updated:

- at least 24 hours;
- 7 days for changes to tasks, TLS, heap, flash writes, OTA or
  authentication.

A fix that touches OTA, authentication or network parsing restarts the soak.

---

## 9. CI runners

- **GitHub-hosted runners (`ubuntu-24.04`) for everything, for now.** The
  repository is public, so standard hosted runners cost no Actions minutes.
  That removes the main reason for self-hosting.
- **Self-hosted option:** the owner has self-hosted runners with the label
  `oob-ci`. If they are ever used here (for example for long nightly fuzz
  runs), jobs pick the runner from a repo variable, with hosted as the
  fallback:

  ```yaml
  runs-on: ${{ github.event_name == 'pull_request' && 'ubuntu-24.04' || vars.CI_RUNNER || 'ubuntu-24.04' }}
  ```

  - **Pull requests always stay on hosted runners.** Fork code never runs on
    the owner's hardware. GitHub advises against self-hosted runners on
    public repositories for exactly this reason, so in addition, require
    approval for workflows from outside contributors.
  - Releases are always built on hosted runners.
  - Runners are registered per repository on a personal account. Using them
    here means registering this repository too, or moving the repositories
    into an organisation and sharing a runner group.
- Long suites (fuzzing, the baseline matrix) are split by measured time, not
  by count.

---

## 10. Working with agents

- Independent parts run in parallel, each in its own worktree and branch,
  with explicit file ownership. For example, logic extraction for `web.cpp`
  and the flasher tests have no files in common.
- Side branches merge into the integration branch, and the integration
  branch merges to `main` through §8.
- Briefs state the hard constraints:
  - push the branch only;
  - no merge, tag, release or workflow dispatch (the orchestrator runs CI);
  - **no flashing, OTA or HTTP calls to any device**;
  - no access to the owner's network;
  - no real feeder data, coordinates or screenshots from the panel.
- Every claim in a report is checked before it is relayed: CI by full SHA, a
  reproduction, or a direct look at the file or image.

---

## 11. Adoption plan

The effort figures are rough, in focused working days. Each phase ends with
its gates blocking on `main`.

| Phase | Content | Effort | Exit |
|---|---|---|---|
| **0** | This document and the `CLAUDE.md` section. The owner decides on: the OTA password/signature question (§5.1), the vendoring of ESP Web Tools, and Pages from the branch vs from Actions. | done / 0.5 | Decisions recorded |
| **1** | CI foundation: the workflow skeleton (SHA-pinned actions, minimal permissions); actionlint, shellcheck, ruff; gitleaks (full history once); `ci/pins.env` plus the arduino-cli compile with the warnings baseline; size budgets; the merged-image and manifest test; `ci/policy.py` (identifiers, neutral defaults, untracked internal docs, version consistency, pins sync, licences and SPDX); fixing what the policy test finds (the `font_*.c` paths, the notices path, three SPDX headers); the harness built on Linux with a headless render of every scenario as a smoke test, plus the `make shots` fix. | 3–4 | All of the above green and blocking |
| **2** | The logic layer and unit tests: extract the §3.1 candidates, starting with the `web.cpp` validators, `mqttParseUri` and the feeder parser; doctest; ASan + UBSan; the 32-bit build; the coverage gate at 80% per file; the §3.3 regression tests. TDD applies to all new code from here on. | 4–6 | Coverage gate blocking |
| **3** | The deterministic harness and baselines: a frozen clock, the pinned container, baselines for every scenario, screen and unit system, the regeneration workflow input, and repaint budgets. | 2–3 | Baselines and repaint budgets blocking |
| **4** | Flasher and analysis: vendor ESP Web Tools, add the CSP; Playwright smoke, axe, phone widths and the CSP-violation test; cppcheck and clang-tidy with baselines. | 2–3 | Gates blocking; the flasher makes no third-party requests |
| **5** | Fuzzing and releases: libFuzzer targets and a synthetic corpus, PR and nightly runs; the release workflow (CI build, `SHA256SUMS`, cosign, provenance, SBOM, notices archive, draft release); `docs/VERIFY.md` and its sync test; the advisory watch; Renovate. | 3–4 | The first CI-built, signed release, verified by following `VERIFY.md` |
| **6** | Ongoing: the HIL record per release; the first full independent security review of the existing code (§7.2); the replay feeder; a host render of the web console for Playwright and axe, and an XSS test with hostile setting values. | per release | — |

Total to the end of phase 5: about **two and a half to four weeks** of
focused work. Phases 1 and 2 give most of the protection; phase 1 alone
would have caught the rule 28 installer and the scenario that `make shots`
never renders.
