# SimonTime

> *"Watch closely. Now do it back."*

A [Pebble Time 2 / Pebble Round 2](https://repebble.com/) watch app — a clone of
the classic **Simon** memory game. Watch the pattern of colors flash, then touch
them back in order. Each round adds one more step and speeds up.

No menus, no words, no network. Just the four colors, a growing sequence, and the
numbers in the middle.

This is a demo touch + speaker game for the color Pebbles.

---

## Two watches, one game

| | Pebble Time 2 (`emery`) | Pebble Round 2 (`gabbro`) |
|---|---|---|
| Screen | rectangular, 200 × 228 | round, 260 × 260 |
| Board | four corner **rectangles** | four **wedges** |
| Sound | ✅ tones + "wamp" | ❌ no speaker — **vibration only** |

The board is drawn native to each screen. Because gabbro has no speaker, the
color tones, the loss sound, and the volume control are compiled out there and
the game plays as a purely **visual** memory game with a vibration on loss. All
sound code is guarded behind `PBL_SPEAKER`.

## Features

- **Touch Simon board** — four color quadrants (Green / Red / Yellow / Blue) in
  the classic "+"-split arrangement, with a circular center display. Big,
  forgiving hit boxes: each quadrant is a quarter of the screen.
- **Watch, then repeat** — the watch flashes the sequence (each step a white
  border + tone), you touch it back. Touching a quadrant flashes its white
  border rather than changing its color.
- **It speeds up** — every round appends one step and shortens the playback,
  smoothly, down to a floor.
- **Authentic Simon tones** (emery) — the original four frequencies
  (415 / 310 / 252 / 209 Hz), synthesized live on-device.
- **Score + high score** — the center hub shows the current depth while playing,
  your final depth at game-over, and your persisted high score (with a ★) while
  idle. Numbers and symbols only — no words anywhere in the app.

## How it works

The tones are **synthesized in real time** on-device (emery only) with a phase
accumulator: `phase_inc = freq / 8000 × 2³²`, indexed into a waveform each
sample, streamed as raw 8 kHz / 16-bit PCM via `speaker_stream_open()` /
`speaker_stream_write()` and refilled from a short timer. The timbre is a
build-time choice — **square** (authentic, the default) or **sine** (a softer
fallback) — set by the `WAVEFORM` constant in `main.c`. The "wamp wamp" loss
sound is two descending, pitch-bent notes made by ramping the phase increment
down. On gabbro all of this is a no-op and a vibration stands in.

The game is a small state machine (`IDLE → PLAYBACK → INPUT → LOSE`) driven by
`AppTimer`s, with a single "last activity" clock powering a per-step input
timeout, a 30-second backlight-off, and a 60-second auto-close. Touch is
suspended via `AppFocusService` while a notification covers the app. Everything —
speeds, timeouts, tone frequencies, floors — lives in named constants at the top
of `main.c`.

## Building

Built with the Pebble **C SDK** (`sdkVersion 3`), targeting `emery` and `gabbro`.

Requires the [Pebble SDK](https://github.com/coredevices/pebble-tool)
(`pebble` CLI). From the project root:

```bash
# Build both platforms.
pebble build

# Run in a local emulator (either platform).
pebble install --emulator emery
pebble install --emulator gabbro
pebble logs --emulator emery
```

> **Note:** the local emulator can't inject touch (only the four buttons) and its
> audio depends on your host, so touch gameplay and sound must be checked on real
> hardware. Press **Select** in the emulator to watch a sequence play back.

The `tools/generate_sine.py` script (needs `numpy`) regenerates
`src/c/simon_synth_tables.h`, the sine table used only by the `WAVEFORM_SINE`
fallback. It is committed, so you only need to re-run it if you change the table.

### On-device

```bash
# In the Pebble mobile app: Settings → Connectivity → Use LAN developer connection.
# Then, Devices → ⋯ → Enable Dev Connect → note the IP.
pebble install --phone <IP_ADDRESS>
pebble logs --phone <IP_ADDRESS>
```

## Controls

| Input | Action |
|---|---|
| Touch a quadrant | (during your turn) repeat that step of the sequence |
| Select | Start a new game (from any state) |
| Up | Volume: mute → 25% → 50% (emery only; unused on gabbro) |
| Down | Unused |
| Back | Exit the app |

The **Up** volume control reveals the current level first; press again within a
few seconds to change it.

## License / attribution

MIT License

All tones are mathematically synthesized on-device — no third-party audio samples
are used. The game *Simon* is a trademark of its respective owner; this is an
independent, non-commercial homage.
