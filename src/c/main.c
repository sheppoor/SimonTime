/*
 * SimonTime - a memory game for Pebble Time 2 (emery) and Pebble Round 2
 * (gabbro). Touch the four color quadrants to repeat an ever-growing sequence.
 *
 * Sound (tones + "wamp" loss) is emery-only (gabbro has no speaker); on gabbro
 * the game is visual + vibration. See PLAN.md for the full spec.
 */

#include <pebble.h>

/* ================================================================== */
/* Tunable constants (PLAN.md Section 13)                              */
/* ================================================================== */
/* Timbre: build-time only. SQUARE is authentic; SINE is a fallback.  */
#define WAVEFORM_SQUARE 0
#define WAVEFORM_SINE   1
#define WAVEFORM        WAVEFORM_SQUARE

/* Tone frequencies (Hz) by quadrant */
#define FREQ_GREEN   415   /* TL */
#define FREQ_RED     310   /* TR */
#define FREQ_YELLOW  252   /* BL */
#define FREQ_BLUE    209   /* BR */

#define TONE_FLOOR_MS   150   /* minimum tone duration for a correct touch */

/* Speed (playback only). step = max(MIN, BASE * FACTOR^(depth-1)). */
#define STEP_ON_BASE_MS    420
#define STEP_ON_MIN_MS     150
#define STEP_GAP_BASE_MS   220
#define STEP_GAP_MIN_MS    90
#define SPEED_FACTOR_NUM   93    /* 0.93 as a fraction */
#define SPEED_FACTOR_DEN   100

/* Pacing */
#define PLAYBACK_END_PAUSE_MS   350
#define ROUND_COMPLETE_PAUSE_MS 500
#define GAME_OVER_MS            7000  /* hold the depth count this long before idle/high score */

/* Timeouts (single activity clock) */
#define INPUT_TIMEOUT_MS     5000
#define BACKLIGHT_TIMEOUT_MS  30000
#define AUTOCLOSE_TIMEOUT_MS  60000
#define HOUSEKEEP_MS          250

/* Volume */
#define VOLUME_OVERLAY_MS  4000
#define VOL_MUTE 0
#define VOL_LOW  25   /* two states only: mute and 25% */

/* Layout */
#define GAP        3    /* half-width of the "+" gap between quadrants */
#define BORDER_W   6    /* white active-border thickness              */

#define MAX_DEPTH  128

/* Persist keys */
#define PKEY_VOLUME 1
#define PKEY_LAST   2
#define PKEY_HIGH   3

/* Quadrant indices: TL=Green, TR=Red, BL=Yellow, BR=Blue */
enum { Q_TL = 0, Q_TR = 1, Q_BL = 2, Q_BR = 3, Q_NONE = -1 };

/* Game states */
typedef enum { ST_IDLE, ST_PLAYBACK, ST_INPUT, ST_GAMEOVER } GameState;

/* ================================================================== */
/* State                                                               */
/* ================================================================== */
static Window *s_window;
static Layer  *s_canvas;

static GameState s_state = ST_IDLE;

static uint8_t s_seq[MAX_DEPTH];
static int     s_depth;        /* current pattern length (the shown count) */
static int     s_play_index;   /* playback cursor                          */
static int     s_input_index;  /* player input cursor                      */

static int  s_active_quad = Q_NONE;   /* quadrant showing the white border */
static bool s_active_auto = false;/* border is from playback/gameover  */
static uint32_t s_touch_down_ms = 0;

static int  s_last_score = 0;
static int  s_high_score = 0;
static bool s_new_high = false;

#if defined(PBL_SPEAKER)
static uint8_t s_volume = VOL_LOW;    /* persisted; volume is emery-only */
static bool    s_vol_overlay = false;
#endif

static bool s_app_in_focus = true;

static uint32_t s_last_activity_ms = 0;
static bool     s_backlight_on = true;

/* Timers */
static AppTimer *s_seq_timer     = NULL;
static AppTimer *s_flash_timer   = NULL;
#if defined(PBL_SPEAKER)
static AppTimer *s_volume_timer  = NULL;
#endif
static AppTimer *s_gameover_timer= NULL;
static AppTimer *s_housekeep_timer = NULL;

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */
static uint32_t now_ms(void) {
  time_t secs = 0;
  uint16_t ms = time_ms(&secs, NULL);
  return (uint32_t)secs * 1000U + ms;
}

static void cancel_timer(AppTimer **t) {
  if (*t) { app_timer_cancel(*t); *t = NULL; }
}

/* Per-quadrant tone frequency + color (one table, indexed by Q_*). Filled at
 * startup because GColor* are compound literals, not const-init friendly. */
typedef struct { int freq; GColor color; } QuadSpec;
static QuadSpec s_quad[4];
static void init_quad_specs(void) {
  s_quad[Q_TL].freq = FREQ_GREEN;  s_quad[Q_TL].color = GColorGreen;
  s_quad[Q_TR].freq = FREQ_RED;    s_quad[Q_TR].color = GColorRed;
  s_quad[Q_BL].freq = FREQ_YELLOW; s_quad[Q_BL].color = GColorYellow;
  s_quad[Q_BR].freq = FREQ_BLUE;   s_quad[Q_BR].color = GColorBlue;
}
static int    freq_for_quad(int q)  { return s_quad[q].freq; }
static GColor color_for_quad(int q) { return s_quad[q].color; }

/* step_on / step_gap for the current depth (multiplicative decay w/ floor). */
static uint32_t scaled_ms(uint32_t base, uint32_t floor_ms, int depth) {
  uint32_t v = base;
  for (int i = 1; i < depth; i++) {
    v = v * SPEED_FACTOR_NUM / SPEED_FACTOR_DEN;
    if (v <= floor_ms) return floor_ms;
  }
  return v < floor_ms ? floor_ms : v;
}
static uint32_t step_on_ms(void)  { return scaled_ms(STEP_ON_BASE_MS,  STEP_ON_MIN_MS,  s_depth); }
static uint32_t step_gap_ms(void) { return scaled_ms(STEP_GAP_BASE_MS, STEP_GAP_MIN_MS, s_depth); }

/* ================================================================== */
/* Audio (emery only - gabbro has no speaker)                          */
/* ================================================================== */
#if defined(PBL_SPEAKER)

#if WAVEFORM == WAVEFORM_SINE
#include "synth_tables.h"
#endif

#define SR              8000
#define BYTES_PER_SAMPLE 2
#define SYNTH_SAMPLES   512
#define BYTES_PER_MS    16
#define REFILL_MS       50
#define TARGET_LEAD_MS  100
#define SQUARE_AMP      16000

static bool     s_synth_open = false;
static uint32_t s_phase_acc = 0;
static uint32_t s_phase_inc = 0;
static uint32_t s_synth_start_ms = 0;
static uint32_t s_bytes_written = 0;
static AppTimer *s_refill_timer = NULL;
static int16_t  s_synth_buf[SYNTH_SAMPLES];

/* Glissando (for the wamp): ramp phase_inc from start to end over a duration. */
static bool     s_gliss = false;
static uint32_t s_inc_start = 0, s_inc_end = 0;
static uint32_t s_note_start_ms = 0, s_note_dur_ms = 0;

static uint32_t inc_for_freq(int freq) {
  return (uint32_t)((double)freq / (double)SR * 4294967296.0);
}

static int16_t synth_sample(void) {
  s_phase_acc += s_phase_inc;
#if WAVEFORM == WAVEFORM_SINE
  return (int16_t)(SINE_TABLE[s_phase_acc >> 24] / 2);
#else
  return (s_phase_acc & 0x80000000u) ? SQUARE_AMP : -SQUARE_AMP;
#endif
}

static void synth_fill(void) {
  if (s_gliss) {
    uint32_t el = now_ms() - s_note_start_ms;
    uint32_t inc;
    if (el >= s_note_dur_ms) {
      inc = s_inc_end;
    } else {
      /* linear lerp start->end */
      int64_t d = (int64_t)s_inc_end - (int64_t)s_inc_start;
      inc = (uint32_t)((int64_t)s_inc_start + d * (int64_t)el / (int64_t)s_note_dur_ms);
    }
    s_phase_inc = inc;
  }
  for (int i = 0; i < SYNTH_SAMPLES; i++) s_synth_buf[i] = synth_sample();
}

static void synth_feed(void) {
  if (!s_synth_open) return;
  uint32_t consumed = (now_ms() - s_synth_start_ms) * BYTES_PER_MS;
  uint32_t target = consumed + (uint32_t)TARGET_LEAD_MS * BYTES_PER_MS;
  int guard = 0;
  while (s_bytes_written < target && guard++ < 64) {
    synth_fill();
    uint32_t want = (uint32_t)SYNTH_SAMPLES * BYTES_PER_SAMPLE;
    uint32_t n = speaker_stream_write(s_synth_buf, want);
    if (n < want) {
      /* rewind phase for the dropped tail so the waveform stays continuous */
      uint32_t dropped = (want - n) / BYTES_PER_SAMPLE;
      s_phase_acc -= dropped * s_phase_inc;
    }
    if (n == 0) break;
    s_bytes_written += n;
    if (n < want) break;
  }
}

static void refill_cb(void *ctx) {
  s_refill_timer = NULL;
  if (!s_synth_open) return;
  synth_feed();
  s_refill_timer = app_timer_register(REFILL_MS, refill_cb, NULL);
}

static void synth_stop(void) {
  cancel_timer(&s_refill_timer);
  if (s_synth_open) { speaker_stop(); s_synth_open = false; }
  s_gliss = false;
  s_bytes_written = 0;
}

static void synth_open_common(void) {
  cancel_timer(&s_refill_timer);
  if (s_synth_open) { speaker_stop(); s_synth_open = false; }
  if (s_volume == VOL_MUTE) return;
  if (!speaker_stream_open(SpeakerPcmFormat_8kHz_16bit, s_volume)) return;
  s_synth_open = true;
  s_phase_acc = 0;
  s_synth_start_ms = now_ms();
  s_bytes_written = 0;
}

static void synth_tone(int freq) {
  synth_open_common();
  if (!s_synth_open) return;
  s_gliss = false;
  s_phase_inc = inc_for_freq(freq);
  synth_feed();
  s_refill_timer = app_timer_register(REFILL_MS, refill_cb, NULL);
}

/* --- Wamp: two descending pitch-bent notes --- */
static AppTimer *s_wamp_timer = NULL;
static int s_wamp_phase = 0;

static void synth_gliss(int f0, int f1, uint32_t dur) {
  synth_open_common();
  if (!s_synth_open) return;
  s_gliss = true;
  s_inc_start = inc_for_freq(f0);
  s_inc_end   = inc_for_freq(f1);
  s_note_start_ms = now_ms();
  s_note_dur_ms = dur;
  s_phase_inc = s_inc_start;
  synth_feed();
  s_refill_timer = app_timer_register(REFILL_MS, refill_cb, NULL);
}

static void wamp_step(void *ctx) {
  s_wamp_timer = NULL;
  s_wamp_phase++;
  switch (s_wamp_phase) {
    case 1: /* gap after note 1 */
      synth_stop();
      s_wamp_timer = app_timer_register(60, wamp_step, NULL);
      break;
    case 2: /* note 2 */
      synth_gliss(247, 165, 400);
      s_wamp_timer = app_timer_register(400, wamp_step, NULL);
      break;
    default: /* done */
      synth_stop();
      break;
  }
}

static void wamp_start(void) {
  cancel_timer(&s_wamp_timer);
  s_wamp_phase = 0;
  synth_gliss(330, 247, 300);   /* note 1 (silent if muted) */
  s_wamp_timer = app_timer_register(300, wamp_step, NULL);
}

#else  /* !PBL_SPEAKER (gabbro): audio is a no-op */
static void synth_tone(int freq) { (void)freq; }
static void synth_stop(void) {}
static void wamp_start(void) {}
#endif

/* ================================================================== */
/* Backlight / activity                                                */
/* ================================================================== */
static void backlight_on(void) {
  if (!s_backlight_on) { light_enable(true); s_backlight_on = true; }
}
static void mark_activity(void) {
  s_last_activity_ms = now_ms();
  backlight_on();
}

/* ================================================================== */
/* Layout                                                              */
/* ================================================================== */
static int s_cx, s_cy, s_R, s_hub_r;

static void compute_layout(GRect b) {
  s_cx = b.size.w / 2;
  s_cy = b.size.h / 2;
  int m = (b.size.w < b.size.h) ? b.size.w : b.size.h;
  s_R = m / 2 - 1;
  s_hub_r = m / 5;
}

#if !defined(PBL_ROUND)
static GRect quad_rect(int q) {
  int w = s_cx * 2, h = s_cy * 2;
  switch (q) {
    case Q_TL: return GRect(0, 0, s_cx - GAP, s_cy - GAP);
    case Q_TR: return GRect(s_cx + GAP, 0, w - (s_cx + GAP), s_cy - GAP);
    case Q_BL: return GRect(0, s_cy + GAP, s_cx - GAP, h - (s_cy + GAP));
    default:   return GRect(s_cx + GAP, s_cy + GAP, w - (s_cx + GAP), h - (s_cy + GAP));
  }
}
#endif

#if defined(PBL_ROUND)
/* Round wedge angle span (Pebble: 0=up, 90=right, 180=down, 270=left). */
static void quad_angles(int q, int *a0, int *a1) {
  switch (q) {
    case Q_TR: *a0 = 0;   *a1 = 90;  break;   /* up..right   */
    case Q_BR: *a0 = 90;  *a1 = 180; break;   /* right..down */
    case Q_BL: *a0 = 180; *a1 = 270; break;   /* down..left  */
    default:   *a0 = 270; *a1 = 360; break;   /* left..up (TL) */
  }
}
#endif

/* ================================================================== */
/* Symbols                                                             */
/* ================================================================== */
static const GPathInfo STAR_INFO = {
  .num_points = 10,
  .points = (GPoint[]) {
    {0, -11}, {3, -4}, {10, -3}, {4, 1}, {6, 9},
    {0, 4}, {-6, 9}, {-4, 1}, {-10, -3}, {-3, -4}
  }
};
static GPath *s_star_path = NULL;

static void draw_star(GContext *ctx, GPoint c, GColor color) {
  graphics_context_set_fill_color(ctx, color);
  gpath_move_to(s_star_path, c);
  gpath_draw_filled(ctx, s_star_path);
}

#if defined(PBL_SPEAKER)
/* speaker icon with 0/1/2 waves (mute = X), centered at c. */
static void draw_volume_symbol(GContext *ctx, GPoint c) {
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_context_set_stroke_width(ctx, 2);
  /* speaker body: small square + cone triangle, left of center */
  int bx = c.x - 10, by = c.y;
  graphics_fill_rect(ctx, GRect(bx, by - 5, 6, 10), 0, GCornerNone);
  GPathInfo cone = { 3, (GPoint[]) { {bx + 6, by - 9}, {bx + 6, by + 9}, {bx + 14, by} } };
  GPath *cp = gpath_create(&cone);
  gpath_draw_filled(ctx, cp);
  gpath_destroy(cp);
  int wx = c.x + 8;
  if (s_volume == VOL_MUTE) {
    graphics_draw_line(ctx, GPoint(wx, by - 6), GPoint(wx + 10, by + 6));
    graphics_draw_line(ctx, GPoint(wx, by + 6), GPoint(wx + 10, by - 6));
  } else {
    /* 25% uses the full two-wave visual (there is no separate "high" state). */
    graphics_draw_arc(ctx, GRect(wx - 8, by - 10, 20, 20), GOvalScaleModeFitCircle,
                      DEG_TO_TRIGANGLE(60), DEG_TO_TRIGANGLE(120));
    graphics_draw_arc(ctx, GRect(wx - 14, by - 16, 32, 32), GOvalScaleModeFitCircle,
                      DEG_TO_TRIGANGLE(60), DEG_TO_TRIGANGLE(120));
  }
}
#endif  /* PBL_SPEAKER */

/* ================================================================== */
/* Rendering                                                           */
/* ================================================================== */
static void draw_quad(GContext *ctx, int q) {
  bool active = (q == s_active_quad);
  GColor color = color_for_quad(q);
#if defined(PBL_ROUND)
  /* Full 90-degree wedges (no angular gap). The separators are straight "+"
   * bars drawn on top in canvas_update_proc, so the gaps look straight rather
   * than crooked radiating lines. */
  int a0, a1;
  quad_angles(q, &a0, &a1);
  GRect full = GRect(s_cx - s_R, s_cy - s_R, s_R * 2, s_R * 2);
  if (active) {
    /* white wedge, then color inset in radius -> solid white rim border */
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_radial(ctx, full, GOvalScaleModeFitCircle, s_R,
                         DEG_TO_TRIGANGLE(a0), DEG_TO_TRIGANGLE(a1));
    int r2 = s_R - BORDER_W;
    GRect inner = GRect(s_cx - r2, s_cy - r2, r2 * 2, r2 * 2);
    graphics_context_set_fill_color(ctx, color);
    graphics_fill_radial(ctx, inner, GOvalScaleModeFitCircle, r2,
                         DEG_TO_TRIGANGLE(a0), DEG_TO_TRIGANGLE(a1));
  } else {
    graphics_context_set_fill_color(ctx, color);
    graphics_fill_radial(ctx, full, GOvalScaleModeFitCircle, s_R,
                         DEG_TO_TRIGANGLE(a0), DEG_TO_TRIGANGLE(a1));
  }
#else
  GRect r = quad_rect(q);
  if (active) {
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, r, 0, GCornerNone);
    GRect inner = GRect(r.origin.x + BORDER_W, r.origin.y + BORDER_W,
                        r.size.w - 2 * BORDER_W, r.size.h - 2 * BORDER_W);
    graphics_context_set_fill_color(ctx, color);
    graphics_fill_rect(ctx, inner, 0, GCornerNone);
  } else {
    graphics_context_set_fill_color(ctx, color);
    graphics_fill_rect(ctx, r, 0, GCornerNone);
  }
#endif
}

#if defined(PBL_ROUND)
/* White border along the active wedge's two straight (inner) edges, drawn as
 * straight bars just outside the "+" gap. Combined with the rim from draw_quad,
 * this gives the active wedge a full, straight-edged border. */
static void draw_round_active_edges(GContext *ctx, int q) {
  graphics_context_set_fill_color(ctx, GColorWhite);
  int g = GAP, bw = BORDER_W, R = s_R, cx = s_cx, cy = s_cy;
  switch (q) {
    case Q_TL:
      graphics_fill_rect(ctx, GRect(cx - g - bw, cy - R, bw, R - g), 0, GCornerNone);
      graphics_fill_rect(ctx, GRect(cx - R, cy - g - bw, R - g, bw), 0, GCornerNone);
      break;
    case Q_TR:
      graphics_fill_rect(ctx, GRect(cx + g, cy - R, bw, R - g), 0, GCornerNone);
      graphics_fill_rect(ctx, GRect(cx + g, cy - g - bw, R - g, bw), 0, GCornerNone);
      break;
    case Q_BL:
      graphics_fill_rect(ctx, GRect(cx - g - bw, cy + g, bw, R - g), 0, GCornerNone);
      graphics_fill_rect(ctx, GRect(cx - R, cy + g, R - g, bw), 0, GCornerNone);
      break;
    case Q_BR:
      graphics_fill_rect(ctx, GRect(cx + g, cy + g, bw, R - g), 0, GCornerNone);
      graphics_fill_rect(ctx, GRect(cx + g, cy + g, R - g, bw), 0, GCornerNone);
      break;
  }
}
#endif

static void draw_hub_number(GContext *ctx, int n, bool star) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", n);
  GFont font = fonts_get_system_font(FONT_KEY_LECO_32_BOLD_NUMBERS);
  GRect box = GRect(s_cx - s_hub_r, s_cy - 20, s_hub_r * 2, 36);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, buf, font, box, GTextOverflowModeFill,
                     GTextAlignmentCenter, NULL);
  if (star) {
    draw_star(ctx, GPoint(s_cx, s_cy - s_hub_r + 8), GColorYellow);
  }
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  for (int q = 0; q < 4; q++) draw_quad(ctx, q);

#if defined(PBL_ROUND)
  /* Straight "+" separators between wedges (a little gap, not crooked). */
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(s_cx - GAP, 0, 2 * GAP, b.size.h), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(0, s_cy - GAP, b.size.w, 2 * GAP), 0, GCornerNone);
  if (s_active_quad != Q_NONE) draw_round_active_edges(ctx, s_active_quad);
#endif

  /* center hub */
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, GPoint(s_cx, s_cy), s_hub_r);

#if defined(PBL_SPEAKER)
  if (s_vol_overlay) {
    draw_volume_symbol(ctx, GPoint(s_cx, s_cy));
    return;
  }
#endif
  switch (s_state) {
    case ST_IDLE:
      draw_hub_number(ctx, s_high_score, s_high_score > 0);
      break;
    case ST_PLAYBACK:
    case ST_INPUT:
      draw_hub_number(ctx, s_depth, false);
      break;
    case ST_GAMEOVER:
      draw_hub_number(ctx, s_last_score, s_new_high);
      break;
  }
}

/* ================================================================== */
/* Persistence                                                         */
/* ================================================================== */
static void load_persist(void) {
#if defined(PBL_SPEAKER)
  s_volume = persist_exists(PKEY_VOLUME) ? (uint8_t)persist_read_int(PKEY_VOLUME) : VOL_LOW;
  if (s_volume != VOL_MUTE && s_volume != VOL_LOW) s_volume = VOL_LOW;
#endif
  s_last_score = persist_exists(PKEY_LAST) ? persist_read_int(PKEY_LAST) : 0;
  s_high_score = persist_exists(PKEY_HIGH) ? persist_read_int(PKEY_HIGH) : 0;
}
static void save_scores(void) {
  persist_write_int(PKEY_LAST, s_last_score);
  persist_write_int(PKEY_HIGH, s_high_score);
}

/* ================================================================== */
/* Game flow                                                           */
/* ================================================================== */
static void cancel_game_timers(void) {
  cancel_timer(&s_seq_timer);
  cancel_timer(&s_flash_timer);
  /* Also the game-over -> idle timer, so starting a new game during the
   * game-over display doesn't get killed when that timer later fires. */
  cancel_timer(&s_gameover_timer);
}

static void enter_idle(void) {
  cancel_game_timers();
  synth_stop();
  s_state = ST_IDLE;
  s_active_quad = Q_NONE;
  s_last_activity_ms = now_ms();
  layer_mark_dirty(s_canvas);
}

static void play_on(void *ctx);

static void enter_input(void) {
  s_state = ST_INPUT;
  s_input_index = 0;
  s_active_quad = Q_NONE;
  s_last_activity_ms = now_ms();  /* fresh per-step input clock */
  layer_mark_dirty(s_canvas);
}
static void input_cb(void *ctx) { s_seq_timer = NULL; enter_input(); }

static void play_off(void *ctx) {
  s_seq_timer = NULL;
  synth_stop();
  s_active_quad = Q_NONE;
  layer_mark_dirty(s_canvas);
  s_play_index++;
  if (s_play_index >= s_depth) {
    s_seq_timer = app_timer_register(PLAYBACK_END_PAUSE_MS, input_cb, NULL);
  } else {
    s_seq_timer = app_timer_register(step_gap_ms(), play_on, NULL);
  }
}

static void play_on(void *ctx) {
  s_seq_timer = NULL;
  int q = s_seq[s_play_index];
  s_active_quad = q;
  s_active_auto = true;
  synth_tone(freq_for_quad(q));
  layer_mark_dirty(s_canvas);
  s_seq_timer = app_timer_register(step_on_ms(), play_off, NULL);
}

static void enter_playback(void) {
  cancel_game_timers();
  synth_stop();
  s_state = ST_PLAYBACK;
  s_play_index = 0;
  s_active_quad = Q_NONE;
  backlight_on();
  layer_mark_dirty(s_canvas);
  s_seq_timer = app_timer_register(PLAYBACK_END_PAUSE_MS, play_on, NULL);
}

static void enter_idle(void);
static void append_and_replay(void);
static void idle_cb(void *ctx)   { s_gameover_timer = NULL; enter_idle(); }
static void replay_cb(void *ctx) { s_seq_timer = NULL; append_and_replay(); }

static int random_quad(void) { return rand() & 0x3; }

static void start_new_game(void) {
  cancel_game_timers();
  synth_stop();
  srand((unsigned)now_ms());
  s_depth = 1;
  s_new_high = false;
  s_seq[0] = random_quad();
  mark_activity();
  enter_playback();
}

static void append_and_replay(void) {
  if (s_depth < MAX_DEPTH) {
    s_seq[s_depth] = random_quad();
    s_depth++;
  }
  enter_playback();
}

static void lose(int correct_quad) {
  cancel_game_timers();
  synth_stop();
  s_state = ST_GAMEOVER;

  s_last_score = s_depth;   /* the depth reached when the game ends */
  s_new_high = (s_last_score > s_high_score);
  if (s_new_high) s_high_score = s_last_score;
  save_scores();

  wamp_start();                 /* emery: sound (silent if muted) */
  if (s_new_high) {             /* distinct celebratory buzz for a record */
    static const uint32_t kCelebrate[] = { 60, 60, 60, 60, 220 };
    vibes_enqueue_custom_pattern((VibePattern){
      .durations = kCelebrate, .num_segments = ARRAY_LENGTH(kCelebrate) });
  } else {
    vibes_double_pulse();       /* normal loss buzz (both platforms, always) */
  }

  s_active_quad = correct_quad; /* held highlight of the correct answer */
  s_active_auto = true;
  backlight_on();
  layer_mark_dirty(s_canvas);

  cancel_timer(&s_gameover_timer);
  s_gameover_timer = app_timer_register(GAME_OVER_MS, idle_cb, NULL);
}

/* ================================================================== */
/* Input (touch)                                                       */
/* ================================================================== */
static int hit_test(int16_t x, int16_t y) {
  int dx = x - s_cx, dy = y - s_cy;
  if (dx * dx + dy * dy <= s_hub_r * s_hub_r) return Q_NONE;  /* hub is dead */
  bool left = x < s_cx, top = y < s_cy;
  if (top)  return left ? Q_TL : Q_TR;
  return left ? Q_BL : Q_BR;
}

static void stop_input_tone(void *ctx) {
  s_flash_timer = NULL;
  synth_stop();
  if (!s_active_auto) s_active_quad = Q_NONE;
  layer_mark_dirty(s_canvas);
}

static void touch_handler(const TouchEvent *event, void *context) {
  if (!s_app_in_focus) return;

  /* When no game is in progress (idle or on the game-over screen), a touch in
   * the center hub starts a new game — the same as the Select button. */
  if (s_state == ST_IDLE || s_state == ST_GAMEOVER) {
    if (event->type == TouchEvent_Touchdown &&
        hit_test(event->x, event->y) == Q_NONE) {
      mark_activity();
      start_new_game();
    }
    return;
  }
  if (s_state != ST_INPUT) return;              /* PLAYBACK: input locked */

  switch (event->type) {
    case TouchEvent_Touchdown: {
      int q = hit_test(event->x, event->y);
      if (q == Q_NONE) return;
      mark_activity();
      /* feedback */
      cancel_timer(&s_flash_timer);
      s_active_quad = q;
      s_active_auto = false;
      s_touch_down_ms = now_ms();
      synth_tone(freq_for_quad(q));
      layer_mark_dirty(s_canvas);
      /* commit the guess on touchdown */
      if (q == s_seq[s_input_index]) {
        s_input_index++;
        if (s_input_index >= s_depth) {
          s_state = ST_PLAYBACK;                /* lock input during the pause (avoids
                                                 * an out-of-bounds read of s_seq) */
          s_seq_timer = app_timer_register(ROUND_COMPLETE_PAUSE_MS, replay_cb, NULL);
        }
      } else {
        lose(s_seq[s_input_index]);
      }
      break;
    }
    case TouchEvent_Liftoff: {
      if (s_active_auto || s_active_quad == Q_NONE) return;
      uint32_t held = now_ms() - s_touch_down_ms;
      if (held < TONE_FLOOR_MS) {
        s_flash_timer = app_timer_register(TONE_FLOOR_MS - held, stop_input_tone, NULL);
      } else {
        stop_input_tone(NULL);
      }
      break;
    }
    default: break;
  }
}

/* ================================================================== */
/* Buttons                                                             */
/* ================================================================== */
#if defined(PBL_SPEAKER)
static uint8_t next_volume(uint8_t v) {
  return (v == VOL_MUTE) ? VOL_LOW : VOL_MUTE;  /* two states: mute <-> 25% */
}

static void hide_volume_cb(void *ctx) {
  s_volume_timer = NULL;
  s_vol_overlay = false;
  layer_mark_dirty(s_canvas);
}

static void up_click(ClickRecognizerRef r, void *ctx) {
  mark_activity();
  if (!s_vol_overlay) {
    s_vol_overlay = true;                 /* reveal only */
  } else {
    s_volume = next_volume(s_volume);     /* cycle */
    persist_write_int(PKEY_VOLUME, s_volume);
  }
  cancel_timer(&s_volume_timer);
  s_volume_timer = app_timer_register(VOLUME_OVERLAY_MS, hide_volume_cb, NULL);
  layer_mark_dirty(s_canvas);
}
#endif  /* PBL_SPEAKER */

static void select_click(ClickRecognizerRef r, void *ctx) {
  mark_activity();
  start_new_game();
}

static void click_config(void *context) {
#if defined(PBL_SPEAKER)
  window_single_click_subscribe(BUTTON_ID_UP, up_click);  /* volume (emery only) */
#endif
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  /* Down unused; Back = default OS exit. */
}

/* ================================================================== */
/* Focus (notifications / overlays)                                    */
/* ================================================================== */
static void focus_will_change(bool in_focus) {
  if (in_focus) return;
  /* An overlay is covering us: stop everything and drop to IDLE so we are not
   * frozen mid-PLAYBACK with no live timer (which would also defeat auto-close).
   * We do not auto-resume; the player presses Select to play again. */
  cancel_game_timers();
  cancel_timer(&s_gameover_timer);
  synth_stop();
  s_state = ST_IDLE;
  s_active_quad = Q_NONE;
  s_active_auto = false;
#if defined(PBL_SPEAKER)
  cancel_timer(&s_volume_timer);
  s_vol_overlay = false;
#endif
  s_app_in_focus = false;
  layer_mark_dirty(s_canvas);
}
static void focus_did_change(bool in_focus) {
  if (in_focus) s_app_in_focus = true;
}

/* ================================================================== */
/* Housekeeping (single activity clock)                                */
/* ================================================================== */
static void housekeep_cb(void *ctx) {
  s_housekeep_timer = app_timer_register(HOUSEKEEP_MS, housekeep_cb, NULL);
  uint32_t now = now_ms();

  if (s_state == ST_PLAYBACK || s_state == ST_GAMEOVER) {
    s_last_activity_ms = now;   /* watch is busy: keep lit, don't time out */
    backlight_on();
    return;
  }
  uint32_t elapsed = now - s_last_activity_ms;

  if (s_state == ST_INPUT && elapsed > INPUT_TIMEOUT_MS) {
    lose(s_seq[s_input_index]);
    return;
  }
  if (s_backlight_on && elapsed > BACKLIGHT_TIMEOUT_MS) {
    light_enable(false);
    s_backlight_on = false;
  }
  if (elapsed > AUTOCLOSE_TIMEOUT_MS) {
    window_stack_pop(true);   /* sole window -> app exits */
  }
}

/* ================================================================== */
/* Window lifecycle                                                    */
/* ================================================================== */
static void main_window_load(Window *window) {
  init_quad_specs();
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  compute_layout(bounds);

  s_star_path = gpath_create(&STAR_INFO);

  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_update_proc);
  layer_add_child(root, s_canvas);

  light_enable(true);
  s_backlight_on = true;
  s_last_activity_ms = now_ms();

  if (touch_service_is_enabled()) {
    touch_service_subscribe(touch_handler, NULL);
  }
  app_focus_service_subscribe_handlers((AppFocusHandlers){
    .will_focus = focus_will_change,
    .did_focus  = focus_did_change,
  });

  s_housekeep_timer = app_timer_register(HOUSEKEEP_MS, housekeep_cb, NULL);
  enter_idle();
}

static void main_window_unload(Window *window) {
  cancel_game_timers();
  synth_stop();
#if defined(PBL_SPEAKER)
  cancel_timer(&s_volume_timer);
#endif
  cancel_timer(&s_gameover_timer);
  cancel_timer(&s_housekeep_timer);
  light_enable(false);
  touch_service_unsubscribe();
  app_focus_service_unsubscribe();
  if (s_star_path) { gpath_destroy(s_star_path); s_star_path = NULL; }
  layer_destroy(s_canvas);
}

static void init(void) {
  load_persist();
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_click_config_provider(s_window, click_config);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = main_window_load,
    .unload = main_window_unload,
  });
  window_stack_push(s_window, true);
}

static void deinit(void) {
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
