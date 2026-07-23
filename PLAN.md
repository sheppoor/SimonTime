# SimonTime — Implementation Plan (v1.0)

A clone of the classic **Simon** memory game for Pebble, targeting the
**Pebble Time 2** (emery) and the **Pebble Round 2** (gabbro). Touch-driven,
on-device tone synthesis, no network.

> This plan is the agreed spec from the design interview. It is written to be
> implemented in the same style as the Touch Tone build. **Read
> `notes/learnings.md` from the Touch Tone project before implementing** — the
> speaker/streaming, phase-accumulator synth, touch, AppFocusService, timer, and
> emulator/deploy lessons all apply here.

App UUID: `6faaec38-d246-419e-852c-ce5295a85e97`
Launcher name: **SimonTime**  ·  `sdkVersion 3`  ·  targets `["emery", "gabbro"]`

---

## 1. Target platforms & the capability split

| | emery (Pebble Time 2) | gabbro (Pebble Round 2) |
|---|---|---|
| Shape | **rect** `PBL_RECT` | **round** `PBL_ROUND` |
| Display | 200 × 228 | 260 × 260 |
| Color | yes | yes |
| Touch | ✅ `PBL_TOUCH` | ✅ `PBL_TOUCH` |
| **Speaker** | ✅ `PBL_SPEAKER` | **❌ none** |
| Backlight | RGB | mono |

**Consequence — one capability-guarded codebase:**
- All **sound** (color tones, wamp, volume system) is **emery-only**, compiled/guarded
  behind `PBL_SPEAKER`. On gabbro the game is fully playable as a **visual** Simon.
- **Vibration** substitutes where sound can't: it fires on **loss** on *both*
  platforms (see §8) and is the only loss cue on gabbro.
- Layout branches on `PBL_RECT` / `PBL_ROUND` (see §3).

---

## 2. The board & colors

Four quadrants in the **true-Simon "+"-split** arrangement (split by the vertical
and horizontal center lines), colors in the canonical positions, always visible:

```
 ┌───────────┬───────────┐
 │  GREEN    │   RED     │      Green  → top-left     (TL)
 │  (TL)     │   (TR)    │      Red    → top-right    (TR)
 ├───────────┼───────────┤      Yellow → bottom-left  (BL)
 │  YELLOW   │   BLUE    │      Blue   → bottom-right (BR)
 │  (BL)     │   (BR)    │
 └───────────┴───────────┘
        + a circular CENTER HUB carved out of the middle
```

- Colors: `GColorRed`, `GColorGreen`, `GColorBlue`, `GColorYellow` (validate on
  e-paper per learnings §1).
- A thin dark **"+" gap** separates the quadrants.
- **Center hub:** a **circle** at the crossing, filled with the background color,
  holding the numeric display (§11/§13). **Non-touch** (a dead zone).
  - `RENDER` risk: at low pixel density the hub may not read as circular — after
    the first on-device build, be prepared to switch it to a diamond/other shape.
    Keep the hub shape behind a single constant/branch.

**Active-state visual (the whole visual language):** a quadrant is "active" — during
the computer's **playback** *and* while the player **touches** it — by drawing a
**white border** on that quadrant for the active duration, plus its tone (emery).
Nothing brightens or changes color. This one vocabulary is how the player reads the
sequence.

### Layout per platform
- **emery (rect):** four **corner rectangles** tiling the 200×228 screen, split by
  the "+" gap; the center hub circle takes a quarter-circle bite out of each rect's
  inner corner.
- **gabbro (round):** four **quarter-circle wedges** filling the 260×260 disc, split
  by the "+" gap, with the center hub as an inner circle (classic round Simon).

### Hit-testing (generous)
Any touch resolves to the quadrant containing its point, relative to center
`(cx, cy)`: `left = x < cx`, `top = y < cy` → TL/TR/BL/BR. The whole screen splits on
the "+", so each zone is ~a quarter of the display. Touches inside the center-hub
circle are ignored (non-input). No angular math needed for the "+" split.

---

## 3. Tones (emery only)

On-device synthesis via a **phase accumulator** (per Touch Tone learnings §2): a
32-bit accumulator per voice, `phase_inc = freq / 8000 * 2^32`, 8 kHz / 16-bit PCM,
streamed with the proactive-AppTimer refill pattern. **Single voice** (one tone at a
time). Persist phase only within a tone; reset at note start (zero-crossing start,
no click).

**Timbre — build-time constant `WAVEFORM`:**
- `WAVEFORM_SQUARE` (**default, authentic**): output = sign of the phase MSB.
- `WAVEFORM_SINE` (internal fallback): 256-entry sine table (generated like Touch
  Tone). Used only if the square wave sounds harsh/aliased on the small speaker.
- **Not** a runtime/user control. Down stays unused. Switch by rebuild.

**Authentic frequencies** (from the MB Simon teardown — square wave), mapped to the
canonical positions:

| Color / position | Freq | Note |
|---|---|---|
| Green (TL) | **415 Hz** | G#4 |
| Red (TR) | **310 Hz** | D#4 |
| Yellow (BL) | **252 Hz** | B3 |
| Blue (BR) | **209 Hz** | G#3 |

All four are constants (`SIMON_FREQ_GREEN` …) — tunable.

**Tone durations:**
- **Playback:** each step's tone lasts `step_on(depth)` (§7), scaled by speed.
- **Input:** the tone plays from touchdown until liftoff, with a **minimum floor
  `TONE_FLOOR_MS` (~120 ms, variable)** so a fast tap still produces a clear blip
  (floor technique from Touch Tone). Use `stop_audio()`-style teardown (cancel the
  refill timer, not a bare `speaker_stop()`), per learnings §2.

---

## 4. The "wamp wamp" loss sound (emery)

Two **descending, pitch-bent** notes ("sad trombone"), synthesized by ramping the
phase increment down across each note:
- Note 1: glissando ~**330 → 247 Hz** over ~**300 ms**.
- short gap (~**60 ms**).
- Note 2: glissando ~**247 → 165 Hz** over ~**400 ms**.

- Uses the build-time `WAVEFORM` (square by default), so it matches the game voice.
- **Respects the volume state** for the *sound* (silent at mute).
- Fires together with the loss **vibration** and the correct-quadrant highlight (§5).

All frequencies/durations are tunable constants.

---

## 5. Game state machine

```
        ┌────────────────────────── Select (any state) ─────────────────────────┐
        │                                                                        v
   ┌─────────┐  Select   ┌──────────┐  demo done   ┌────────┐  correct full   ┌────────┐
   │  IDLE   │ ───────►  │ PLAYBACK │ ───────────► │ INPUT  │ ─── repeat ───► (depth++,│
   │ (hub =  │           │(replay   │  (+ pause)   │(player │                  append) │
   │  hi-    │           │ whole    │              │ repeats│                  └───┬────┘
   │  score) │           │ sequence)│              │ seq)   │                      │
   └────▲────┘           └──────────┘              └───┬────┘   ── back to PLAYBACK ┘
        │                                              │
        │        after ~GAME_OVER_MS                   │ wrong quadrant OR input-timeout
        │                                              v
        │                                        ┌──────────┐
        └──────────────────────────────────────  │  LOSE    │
                                                  └──────────┘
```

- **IDLE (attract):** board shown; hub = **high score + star** (§11). **Select**
  starts a game; **Back** exits.
- **PLAYBACK:** replay the *entire* current sequence from the start — each step =
  white-border flash + tone for `step_on(depth)`, with `step_gap(depth)` between
  steps (§7). Hub shows the **depth count**. **Touch is ignored.** Then a
  `PLAYBACK_END_PAUSE` before input opens.
- **INPUT (player's turn):** player repeats the sequence.
  - Guess **commits on touchdown**; correctness evaluated immediately.
  - Correct step → continue. Correct **full** sequence → `ROUND_COMPLETE_PAUSE`,
    depth++, append **one new random step**, → PLAYBACK.
  - **Wrong quadrant** or **per-step input timeout** → LOSE.
  - No input queue; fast successive presses are fine.
- **LOSE:** simultaneously — **wamp** (emery) + **vibration** (both, even when muted)
  + **hold a white border on the correct quadrant** (the expected next step; visual
  only, no note) + hub shows **final score** (with **star + celebratory flash/vibe**
  if it beats the high score). Hold `GAME_OVER_MS` (~2 s), then → IDLE.
- **Select restarts from any state** (abandons the current game). **Start depth = 1.**

Input is **locked during PLAYBACK** and during the LOSE beat.

---

## 6. Timeouts, activity & backlight

A single **`s_last_activity_ms`** timestamp, reset by **any** touch or button. The
watch's own activity during PLAYBACK and the LOSE animation also counts as activity
(keep the screen lit; don't let inactivity timers fire mid-demo).

| Timer | Scope | Fires → | Default (variable) |
|---|---|---|---|
| **Input-loss** | INPUT only, **per-step** (resets each correct touch) | LOSE | `INPUT_TIMEOUT_MS = 5000` |
| **Backlight-off** | any state | backlight off | `BACKLIGHT_TIMEOUT_MS = 30000` |
| **Auto-close** | any state | exit app | `AUTOCLOSE_TIMEOUT_MS = 60000` |

- Input-loss is **fixed** (independent of speed) — a safety timeout, not difficulty.
- **Backlight:** force on at launch with `light_enable(true)`; our 30 s timer turns
  it off (`light_enable(false)`); any activity re-enables + resets. (Note: gabbro has
  a mono backlight — `light_enable` works; verify on device.) This is *not* the
  system `light_enable_interaction()` (that uses the OS timeout).
- **Back** exits immediately regardless of timers.
- Because input-loss (5 s) ≪ 30 s/60 s, the two long timers effectively only fire in
  IDLE and game-over (sitting untouched).

---

## 7. Speed scaling (playback only)

Multiplicative decay with a floor — **all tunable** (expect to adjust from testing):

```
step_on(depth)  = max(STEP_ON_MIN,  STEP_ON_BASE  * SPEED_FACTOR^(depth-1))
step_gap(depth) = max(STEP_GAP_MIN, STEP_GAP_BASE * SPEED_FACTOR^(depth-1))
```

| Constant | Default |
|---|---|
| `STEP_ON_BASE` | 420 ms |
| `STEP_ON_MIN` | 150 ms |
| `STEP_GAP_BASE` | 220 ms |
| `STEP_GAP_MIN` | 90 ms |
| `SPEED_FACTOR` | 0.93 per depth increment |

Speed affects **playback demonstration only**. The player's input feedback is tied to
their actual touch duration (with the `TONE_FLOOR_MS` floor), not the speed curve.

Use integer math / a small lookup or repeated multiply; avoid float in hot paths
where practical (a per-round recompute is fine).

---

## 8. Volume & the volume overlay (emery)

Three states, cycling: **mute (0) → low (25%) → high (50%) → mute → …**
- Applied via the `volume` arg to `speaker_stream_open` (0–100). Mute = skip playback
  entirely. Default at first launch = **low (25%)**. **Persisted** (§11).

**Up button — reveal-then-change:**
- **First Up press (overlay hidden):** *reveal* the current volume in the hub for
  `VOLUME_OVERLAY_MS = 4000` (variable). **Does not change** the volume.
- **Up press while the overlay is visible:** cycle to the next state **and restart**
  the 4 s window.
- After the window elapses with no Up press, the overlay disappears; hub returns to
  count/score.
- Up works in **any** state; the overlay just covers the hub for those 4 s. Quadrants
  remain visible/touchable.

**Display (no letters):** a **speaker icon drawn geometrically** with **0 / 1 / 2
"waves"** — mute = speaker + slash/X, low = one wave, high = two waves (drawn like the
`*`/`#` glyphs in Touch Tone).

**Loss vibration** (both platforms, **always**, even at mute): a distinctive pattern
(e.g. `vibes_double_pulse()` or a custom two-pulse). Sole loss cue on gabbro.

---

## 9. Scoring, persistence, RNG

- **Score = the depth reached** when the game ends.
- **Persisted** (Pebble `persist_*` storage): **volume state**, **last score**,
  **high score**.
- **Hub content (LECO numbers font, §13):**
  - **IDLE:** high score + **star** symbol.
  - **PLAYBACK / INPUT:** current depth count.
  - **Game-over:** final score; **+ star + celebratory flash/vibe** if it's a new high.
  - **Volume overlay:** speaker-waves symbol (covers the hub for its 4 s).
- **Star** = "high score," drawn geometrically wherever it appears.
- **RNG:** seed from the real-time clock (`time()` / `time_ms`) at **each new game** so
  sequences differ every play. Append one random quadrant (0–3) per round. No
  reproducible-seed requirement. Sequence stored as a byte array (max depth bounded;
  pick a generous cap, e.g. 128).

---

## 10. Rendering / numbers & symbols

**Global rule: no words/letters anywhere in-app.** Only numbers and symbols in the
center hub.
- **Digits** (count / score): `FONT_KEY_LECO_*_NUMBERS` system font (digits-only),
  sized per platform (larger on gabbro's 260² hub). No custom font resource.
- **Symbols drawn geometrically:** the star (high score), the speaker-with-waves
  (volume), and any other glyphs — using `graphics_draw_*` (as `*`/`#` were drawn in
  Touch Tone). `GColorFromRGB` is not a constant expression — use macros/runtime.
- The white active-border is drawn per quadrant (rect edge inset on emery; wedge arc
  on gabbro).

---

## 11. Buttons & focus

- **Select:** start a new game (from any state).
- **Up:** volume overlay reveal/cycle (§8).
- **Down:** **unused** (reserved).
- **Back:** default OS behavior — single window, so Back exits. Do **not** subscribe
  `BUTTON_ID_BACK`.
- **AppFocusService** (per Touch Tone learnings §5): on focus loss (a notification /
  overlay covers the app), stop audio, cancel game timers, and block touch via an
  `s_app_in_focus` guard at the top of the touch handler. Do not auto-resume; the
  overlay steals input while up. (Guards against the touch-leak-through behavior.)

---

## 12. Project structure & setup

```
simontime/
├── package.json            # name "SimonTime", uuid, sdkVersion 3, targets [emery, gabbro], resources
├── wscript                 # standard waf build (glob src/c/**/*.c)
├── src/c/
│   ├── main.c              # game loop, state machine, input, timers, rendering
│   ├── synth.c/.h          # phase-accumulator tone + wamp synthesis (emery)  [or fold into main.c]
│   └── (generated sine table header, if WAVEFORM_SINE is used — via tools/)
├── resources/images/
│   └── app_icon.png        # four-color quadrant mini-board
└── tools/
    └── generate_sine.py    # (optional) sine table for the sine-timbre fallback
```

- `package.json`: modern format (top-level name/author/version + `pebble` object),
  `targetPlatforms: ["emery", "gabbro"]`, app icon resource with `menuIcon: true`.
  (Old formats error "very outdated" — learnings §8.)
- App is loaded entirely into RAM; keep tables modest (learnings §1).

---

## 13. Constants reference (all tunable)

| Group | Constant | Default |
|---|---|---|
| Timbre | `WAVEFORM` | `SQUARE` (sine = fallback) |
| Tones | `SIMON_FREQ_{GREEN,RED,YELLOW,BLUE}` | 415 / 310 / 252 / 209 Hz |
| Tone floor | `TONE_FLOOR_MS` | 120 ms |
| Speed | `STEP_ON_BASE / _MIN` | 420 / 150 ms |
| Speed | `STEP_GAP_BASE / _MIN` | 220 / 90 ms |
| Speed | `SPEED_FACTOR` | 0.93 |
| Pacing | `PLAYBACK_END_PAUSE` | ~300 ms |
| Pacing | `ROUND_COMPLETE_PAUSE` | ~500 ms |
| Pacing | `GAME_OVER_MS` | ~2000 ms |
| Timeouts | `INPUT_TIMEOUT_MS` | 5000 ms |
| Timeouts | `BACKLIGHT_TIMEOUT_MS` | 30000 ms |
| Timeouts | `AUTOCLOSE_TIMEOUT_MS` | 60000 ms |
| Volume | `VOLUME_OVERLAY_MS` | 4000 ms |
| Volume | `VOL_LOW / VOL_HIGH` | 25 / 50 |
| Wamp | note freqs / durations / gap | 330→247 / 247→165 Hz, 300/400/60 ms |
| Layout | quadrant gap, hub radius, border width, hub shape | tune on device |

---

## 14. Implementation phases

0. **Project skeleton** — package.json (both targets), wscript, app icon, builds
   empty on emery + gabbro.
1. **Board rendering** — per-platform layout (rect quadrants / round wedges), "+"
   gap, circular hub, colors. Verify in emulator on both platforms via screenshot.
2. **Hit-testing + active border** — touchdown → quadrant → white border while held;
   emery only in emulator by review (no touch injection — learnings §3).
3. **Tone synthesis (emery)** — phase-accumulator square tones per color; verify
   frequencies offline (FFT) per learnings §11; on-device audio pass.
4. **Game loop** — IDLE→PLAYBACK→INPUT→LOSE state machine, sequence grow/replay,
   scoring, RNG seed.
5. **Speed scaling + pacing** — depth-scaled playback timing.
6. **Timeouts + backlight** — activity clock, input-loss, 30 s backlight, 60 s close.
7. **Volume system + overlay** — reveal-then-change, speaker-waves symbol, persistence.
8. **Wamp + loss presentation** — descending-bend wamp, vibration (both), correct-
   quadrant highlight, game-over/high-score star.
9. **AppFocusService** guard.
10. **Persistence** — volume / last / high score.
11. **On-device validation** — the touch, audio, backlight, vibration, and color
    checklist on **both** real watches (emulator can't test touch/audio — learnings
    §9/§11). gabbro pass confirms the silent + vibration + visual path.

---

## 15. To verify on device (not decisions — confirm during build)

- Exact rendering of the circular hub at low pixel density (fall back to diamond if
  it doesn't read as round).
- `light_enable(true/false)` behavior and battery impact on gabbro (mono backlight).
- Square-wave harshness/aliasing on the small emery speaker → switch `WAVEFORM` to
  sine if needed.
- Touch hit-box feel (generosity) and the touchdown-commit responsiveness.
- Vibration pattern legibility as the loss cue on gabbro.

---

## 16. Sources

- Simon tone frequencies (reverse-engineered from an original MB unit):
  <https://www.waitingforfriday.com/?p=586>
- Platform capabilities: SDK `pebble_sdk_platform.py` (`PBL_TOUCH`, `PBL_SPEAKER`,
  `PBL_ROUND`, display sizes).
- Prior-build lessons: `notes/learnings.md` (Touch Tone).
