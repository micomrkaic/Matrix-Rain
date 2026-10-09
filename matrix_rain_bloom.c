/*
 * This file is part of Mico's Matrix Rain C code
 *
 * Mico's Matrix Rain C code is free software:
 * you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 *  Mico's Matrix Rain C code is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with  Mico's Matrix Rain C code. If not, see <https://www.gnu.org/licenses/>.
 */

// matrix_rain_bloom.c — SDL2 + OpenGL 3.3 core + SDL_ttf glyph atlas with bloom
//
// Features
// - Half-width katakana (mirrored) mixed with digits and a few Latin glyphs,
//   or plain ASCII (-G). Katakana come from a CJK font found automatically.
// - Glyphs in the trails mutate at random (--mutate).
// - Up to three parallax layers (--layers): far layers are smaller, dimmer,
//   slower and glow less.
// - White-hot heads that cool into the palette colour as the trail ages.
// - Message mode (-M "text"): streams periodically freeze into your text.
// - Shader pipeline (GL 3.3 core; shaders are GLSL ES 3.0-compatible for a
//   future WebGL2 build). All glyphs go out in one batched draw per pass.
// - Bloom from the stream heads: bright pass → separable Gaussian blur
//   (ping-pong FBOs) → additive composite.
// - Continuous exponential trail fades, decoupled from the simulation rate.
// - Palettes (-C) + live hue cycling (--cycle)
// - Optional X11 desktop mode (compile with -DENABLE_X11_DESKTOP)
//
// Build
//   Linux:  sudo apt install libsdl2-dev libsdl2-ttf-dev; make   (make DESKTOP=1)
//   macOS:  brew install sdl2 sdl2_ttf; make
//   Web:    emmake make web   → dist/ (WebGL2; options come from the URL query,
//           e.g. index.html?C=matrix&layers=3&M=WAKE+UP+NEO)
//
// Timing
//   -f sets the rain speed in simulation steps per second (default 60).
//   Rendering is paced by vsync / the display refresh rate, independently.
//   -s and -P are in logical points; they are scaled on HiDPI displays.
//
// Run examples
//   ./matrix_rain_bloom -C matrix
//   ./matrix_rain_bloom -C green -M "WAKE UP" --message-every 20
//   ./matrix_rain_bloom -G ascii --layers 1 --mutate 0
//   ./matrix_rain_bloom --cycle --cycle-speed 25 --bloom-intensity 1.2
//   ./matrix_rain_bloom --desktop -C matrix   # requires make DESKTOP=1
//
// Keys
//   q quit • p pause • + / - speed • [ / ] density • m mono • F11 fullscreen
//   c hue cycling • , / . cycle speed −/+ • n / b palettes • 1..7 quick palettes
//   v toggle bloom • g / f bloom intensity +/− • r / e blur radius +/−
//   y / t bloom threshold +/− • l layers 1→2→3 • k glyph set • x show message
//   h (or F1, ?) help overlay with current settings • Esc closes it

#define _XOPEN_SOURCE 700

#include <SDL.h>
#include <SDL_ttf.h>

#if defined(__EMSCRIPTEN__)
  #include <emscripten.h>
  #include <emscripten/html5.h>
  #include <GLES3/gl3.h>
#elif defined(__APPLE__)
  #define GL_SILENCE_DEPRECATION 1
  #include <OpenGL/gl3.h>
#else
  #define GL_GLEXT_PROTOTYPES 1
  #include <GL/gl.h>
  #include <GL/glext.h>
#endif

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#if defined(ENABLE_X11_DESKTOP) && \
    (defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
     defined(__NetBSD__))
  #include <SDL_syswm.h>
  #include <X11/Xatom.h>
  #include <X11/Xlib.h>
#endif

// ---- Tunables ----
#define MAX_TRAIL        12
#define MIN_TRAIL        4
#define MIN_SPEED        1
#define MAX_SPEED        4
#define START_PROB_PCT   6
#define DEFAULT_SIM_HZ   60   // simulation steps per second (rain speed)
#define MIN_SIM_HZ       6
#define MAX_SIM_HZ       240
#define MAX_CATCHUP      8    // max sim steps per rendered frame
#define DEFAULT_DENSITY  65
#define DEFAULT_CELL_PX  18
#define DEFAULT_PT_SIZE  22

// Trail fade: a cell left behind by a head starts at level 1 and decays as
// exp(-t/tau), with tau chosen so it reaches FADE_END after `trail` sim steps.
#define FADE_END         0.04f
#define FADE_CUTOFF      0.01f   // below this the cell is cleared

// Message timing (seconds)
#define MSG_REVEAL_MAX   6.0
#define MSG_HOLD_S       4.0
#define MSG_FIRST_DELAY  2.0

// ===== RNG (xorshift64*) =====
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static inline uint32_t rng_u32(void) {
  uint64_t x = rng_state;
  x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
  rng_state = x;
  return (uint32_t)((x * 0x2545F4914F6CDD1Dull) >> 32);
}

static inline int irand(int lo, int hi) {   // inclusive
  if (hi <= lo) return lo;
  return lo + (int)(((uint64_t)rng_u32() * (uint64_t)(hi - lo + 1)) >> 32);
}

static inline float frand(void) {           // [0, 1)
  return (float)(rng_u32() >> 8) * (1.0f / 16777216.0f);
}

// ===== Glyphs =====
#define MAX_GLYPHS   256
#define NO_GLYPH     0xFFFF
#define KANA_FIRST   0xFF66   // ｦ
#define KANA_LAST    0xFF9D   // ﾝ

typedef struct {
  float    u0, v0, u1, v1;
  int      w, h;
  uint32_t cp;
  bool     kana;
} Glyph;

static GLuint   atlas_tex = 0;
static int      atlas_w = 0, atlas_h = 0;
static Glyph    glyphs[MAX_GLYPHS];
static int      n_glyphs = 0;
static uint16_t ascii_gid[128];             // ASCII code → glyph id
static bool     have_kana = false;

typedef enum { GM_MIX = 0, GM_KANA, GM_ASCII, GM_COUNT } GlyphMode;
static const char *GLYPH_MODE_NAME[GM_COUNT] = { "mix", "kana", "ascii" };
static GlyphMode g_glyph_mode = GM_MIX;
static bool      g_mirror     = true;       // mirror katakana, as in the film
static float     g_mutate     = 1.5f;       // glyph swaps per lit cell per second

static const char *ASCII_POOL = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz@#$%^&*()[]{}<>|/\\;:,.+=-_`~'\"";
static const char *MIX_EXTRA  = "0123456789Z:.\"=*+-<>|";
static const char *KANA_EXTRA = "0123456789";

static uint16_t pool[GM_COUNT][MAX_GLYPHS];
static int      pool_n[GM_COUNT];

// ===== Layers & streams =====
typedef struct {
  bool active;
  int  y;
  int  speed;
  int  tick;
  int  trail;
} Stream;

enum { F_MIRROR = 1, F_LOCKED = 2, F_PENDING = 4 };

#define MAX_LAYERS 3
typedef struct {
  int       rows, cols, cell;   // grid size and cell size in drawable px
  float     scale, dim, speed;  // relative to the front layer
  float    *lvl;                // brightness level 0..1 (0 = empty)
  uint8_t  *trail;              // trail length (sim steps) → fade rate
  uint8_t  *head;               // 1 if a stream head sits here
  uint8_t  *flags;              // F_*
  uint16_t *ch;                 // glyph id
  uint16_t *msg;                // pending message glyph (front layer)
  Stream   *streams;
  double    acc;                // sim-time accumulator
} Layer;

static const float LAYER_SCALE[MAX_LAYERS] = { 1.00f, 0.70f, 0.50f };
static const float LAYER_DIM[MAX_LAYERS]   = { 1.00f, 0.45f, 0.25f };
static const float LAYER_SPEED[MAX_LAYERS] = { 1.00f, 0.70f, 0.50f };

static Layer layers[MAX_LAYERS];
static int   g_layers = 2;

static int   cell_pt     = DEFAULT_CELL_PX; // logical size (user, -s)
static int   cell_px     = DEFAULT_CELL_PX; // physical size = cell_pt * dpi
static float g_dpi_scale = 0.0f;            // drawable px per window pt

// ===== Palette =====
typedef enum {
  PAL_BLUE = 0,
  PAL_GREEN,
  PAL_PURPLE,
  PAL_CYAN,
  PAL_MAGENTA,
  PAL_RED,
  PAL_MATRIX,
  PAL_COUNT
} PaletteId;

static PaletteId g_palette     = PAL_BLUE;  // default
static bool      g_cycle       = false;     // hue cycling enabled
static float     g_hue         = 210.0f;    // degrees
static float     g_cycle_speed = 30.0f;     // deg/s

static float palette_base_hue(PaletteId id) {
  switch (id) {
    case PAL_BLUE:    return 210.f;
    case PAL_GREEN:   return 120.f;
    case PAL_PURPLE:  return 280.f;
    case PAL_CYAN:    return 190.f;
    case PAL_MAGENTA: return 300.f;
    case PAL_RED:     return 0.f;
    case PAL_MATRIX:  return 120.f;
    default:          return 210.f;
  }
}

static const char *PALETTE_NAME[PAL_COUNT] = {
  "blue", "green", "purple", "cyan", "magenta", "red", "matrix"
};

static PaletteId next_palette(PaletteId p) {
  return (PaletteId)((((int)p) + 1) % PAL_COUNT);
}

static PaletteId prev_palette(PaletteId p) {
  return (PaletteId)((((int)p) + PAL_COUNT - 1) % PAL_COUNT);
}

// ===== Glyph pools =====
static void build_pools(void) {
  for (int m = 0; m < GM_COUNT; ++m) pool_n[m] = 0;

  for (const char *p = ASCII_POOL; *p; ++p)
    if (ascii_gid[(unsigned char)*p] != NO_GLYPH)
      pool[GM_ASCII][pool_n[GM_ASCII]++] = ascii_gid[(unsigned char)*p];

  if (!have_kana) {
    memcpy(pool[GM_MIX],  pool[GM_ASCII], sizeof pool[GM_ASCII]);
    memcpy(pool[GM_KANA], pool[GM_ASCII], sizeof pool[GM_ASCII]);
    pool_n[GM_MIX] = pool_n[GM_KANA] = pool_n[GM_ASCII];
    return;
  }

  for (int g = 0; g < n_glyphs; ++g) {
    if (!glyphs[g].kana) continue;
    pool[GM_MIX][pool_n[GM_MIX]++]   = (uint16_t)g;
    pool[GM_KANA][pool_n[GM_KANA]++] = (uint16_t)g;
  }
  for (const char *p = MIX_EXTRA; *p; ++p)
    if (ascii_gid[(unsigned char)*p] != NO_GLYPH)
      pool[GM_MIX][pool_n[GM_MIX]++] = ascii_gid[(unsigned char)*p];
  for (const char *p = KANA_EXTRA; *p; ++p)
    if (ascii_gid[(unsigned char)*p] != NO_GLYPH)
      pool[GM_KANA][pool_n[GM_KANA]++] = ascii_gid[(unsigned char)*p];
}

// Random glyph from the active set; sets/clears the mirror flag.
static inline uint16_t pick_glyph(uint8_t *flags) {
  int m = g_glyph_mode;
  uint16_t g = pool[m][irand(0, pool_n[m] - 1)];
  if (g_mirror && glyphs[g].kana) *flags |= F_MIRROR;
  else                            *flags &= (uint8_t)~F_MIRROR;
  return g;
}

// ===== Layers =====
static void free_layer(Layer *L) {
  free(L->lvl);   free(L->trail); free(L->head);
  free(L->flags); free(L->ch);    free(L->msg);
  free(L->streams);
  memset(L, 0, sizeof *L);
}

static void free_layers(void) {
  for (int i = 0; i < MAX_LAYERS; ++i) free_layer(&layers[i]);
}

static bool alloc_layer(Layer *L, int idx, int w, int h) {
  free_layer(L);
  L->scale = LAYER_SCALE[idx];
  L->dim   = LAYER_DIM[idx];
  L->speed = LAYER_SPEED[idx];
  L->cell  = (int)lroundf((float)cell_px * L->scale);
  if (L->cell < 4) L->cell = 4;
  L->rows = h / L->cell; if (L->rows < 2) L->rows = 2;
  L->cols = w / L->cell; if (L->cols < 2) L->cols = 2;

  size_t n = (size_t)L->rows * (size_t)L->cols;
  L->lvl     = calloc(n, sizeof *L->lvl);
  L->trail   = calloc(n, sizeof *L->trail);
  L->head    = calloc(n, sizeof *L->head);
  L->flags   = calloc(n, sizeof *L->flags);
  L->ch      = calloc(n, sizeof *L->ch);
  L->msg     = calloc(n, sizeof *L->msg);
  L->streams = calloc((size_t)L->cols, sizeof *L->streams);
  if (!L->lvl || !L->trail || !L->head || !L->flags || !L->ch || !L->msg ||
      !L->streams) {
    free_layer(L);
    return false;
  }

  for (int x = 0; x < L->cols; ++x) {
    L->streams[x].active = false;
    L->streams[x].y      = -irand(1, L->rows);
    L->streams[x].speed  = irand(MIN_SPEED, MAX_SPEED);
    L->streams[x].tick   = 0;
    L->streams[x].trail  = irand(MIN_TRAIL, MAX_TRAIL);
  }
  return true;
}

// Fade every non-head, unlocked cell by real elapsed time dt. The rate for a
// cell with trail length T is ln(1/FADE_END) / (T * step_dt), so a trail
// spans T of this layer's steps.
static void fade_layer(Layer *L, double dt, double step_dt) {
  float fac[MAX_TRAIL + 1];
  const float k = logf(1.0f / FADE_END);
  fac[0] = 0.0f;
  for (int t = 1; t <= MAX_TRAIL; ++t)
    fac[t] = expf(-(float)(k * dt / ((double)t * step_dt)));

  int n = L->rows * L->cols;
  for (int i = 0; i < n; ++i) {
    if (L->lvl[i] <= 0.0f || L->head[i] || (L->flags[i] & F_LOCKED)) continue;
    float v = L->lvl[i] * fac[L->trail[i]];
    L->lvl[i] = v < FADE_CUTOFF ? 0.0f : v;
  }
}

static int msg_locked = 0;   // cells of the current message already revealed

static void step_layer(Layer *L, int density_pct, double step_dt) {
  const int rows = L->rows, ncols = L->cols;

  for (int x = 0; x < ncols; ++x) {
    Stream *s = &L->streams[x];

    if (!s->active) {
      int prob = (START_PROB_PCT * density_pct) / 100;
      if (irand(1, 100) <= prob) {
        s->active = true;
        s->y      = -irand(1, rows / 2);
        s->speed  = irand(MIN_SPEED, MAX_SPEED);
        s->tick   = 0;
        s->trail  = irand(MIN_TRAIL, MAX_TRAIL);
      }
      continue;
    }

    if (++s->tick < s->speed) continue;
    s->tick = 0;

    // The cell the head is leaving becomes trail and starts fading
    // (or stays lit if it is part of a revealed message).
    if (s->y >= 0 && s->y < rows) {
      int idx = s->y * ncols + x;
      L->head[idx]  = 0;
      L->lvl[idx]   = 1.0f;
      L->trail[idx] = (uint8_t)s->trail;
    }

    s->y++;
    if (s->y >= rows + irand(1, rows / 3 + 1)) {
      s->active = false;
      continue;
    }

    if (s->y >= 0 && s->y < rows) {
      int idx = s->y * ncols + x;
      L->head[idx]  = 1;
      L->lvl[idx]   = 1.0f;
      L->trail[idx] = (uint8_t)s->trail;
      if (L->flags[idx] & F_PENDING) {          // head reveals a message cell
        L->flags[idx] = F_LOCKED;
        L->ch[idx]    = L->msg[idx];
        ++msg_locked;
      } else if (!(L->flags[idx] & F_LOCKED)) {
        L->ch[idx] = pick_glyph(&L->flags[idx]);
      }
    }
  }

  // Random glyph mutation in the trails.
  float p = g_mutate * (float)step_dt;
  if (p <= 0.0f) return;
  int n = rows * ncols;
  for (int i = 0; i < n; ++i) {
    if (L->lvl[i] <= 0.0f || L->head[i] || (L->flags[i] & F_LOCKED)) continue;
    if (frand() < p) L->ch[i] = pick_glyph(&L->flags[i]);
  }
}

// ===== Message mode =====
typedef enum { MSG_IDLE, MSG_REVEAL, MSG_HOLD } MsgState;

static const char *g_msg       = NULL;
static float       g_msg_every = 15.0f;    // seconds between messages (0 = once)
static MsgState    msg_state   = MSG_IDLE;
static double      msg_timer   = MSG_FIRST_DELAY;
static int        *msg_cells   = NULL;     // cell indices in the front layer
static int         msg_n       = 0;
static bool        msg_shown   = false;

static void msg_reset(double delay) {
  msg_state = MSG_IDLE;
  msg_timer = delay;
  msg_n = 0;
  msg_locked = 0;
}

static void msg_release(void) {
  Layer *L = &layers[0];
  for (int k = 0; k < msg_n; ++k) {
    int i = msg_cells[k];
    if (L->flags[i] & F_LOCKED) {
      L->trail[i] = MAX_TRAIL;            // slow fade-out
      if (glyphs[L->ch[i]].kana && g_mirror) L->flags[i] = F_MIRROR;
      else                                   L->flags[i] = 0;
    }
    L->flags[i] &= (uint8_t)~F_PENDING;
  }
  msg_reset(g_msg_every > 0.0f ? g_msg_every : 1e30);
}

// Lay the message out on the front layer, word-wrapped and centred, and mark
// its cells pending; each one locks when a stream head reaches it.
static void msg_start(void) {
  Layer *L = &layers[0];
  if (!g_msg || !L->lvl) return;
  if (msg_state != MSG_IDLE) msg_release();

  int width = L->cols - 2;
  if (width < 1) width = 1;
  int max_lines = (L->rows + 1) / 2;

  // Greedy word wrap into lines of at most `width` characters.
  enum { MAX_LINES = 64 };
  const char *ls[MAX_LINES]; int ll[MAX_LINES]; int nl = 0;
  const char *p = g_msg;
  while (*p && nl < MAX_LINES && nl < max_lines) {
    while (*p == ' ') ++p;
    if (!*p) break;
    const char *start = p, *last_break = NULL;
    int len = 0;
    while (p[len] && p[len] != '\n' && len < width) {
      if (p[len] == ' ') last_break = p + len;
      ++len;
    }
    if (p[len] && p[len] != ' ' && p[len] != '\n' && last_break) len = (int)(last_break - start);
    while (len > 0 && start[len - 1] == ' ') --len;
    ls[nl] = start; ll[nl] = len; ++nl;
    p = start + len;
    if (*p == '\n') ++p;
  }
  if (nl == 0) { msg_reset(g_msg_every > 0.0f ? g_msg_every : 1e30); return; }

  int block = nl * 2 - 1;                         // blank row between lines
  int top = (L->rows - block) / 2 + irand(-L->rows / 6, L->rows / 6);
  if (top < 0) top = 0;
  if (top + block > L->rows) top = L->rows - block;
  if (top < 0) top = 0;

  free(msg_cells);
  msg_cells = malloc(sizeof *msg_cells * (size_t)(nl * width + 1));
  msg_n = 0; msg_locked = 0;
  if (!msg_cells) { msg_reset(1e30); return; }

  for (int li = 0; li < nl; ++li) {
    int row = top + li * 2;
    if (row >= L->rows) break;
    int col0 = (L->cols - ll[li]) / 2;
    for (int c = 0; c < ll[li]; ++c) {
      unsigned char ch = (unsigned char)ls[li][c];
      if (ch == ' ') continue;
      uint16_t g = (ch < 128) ? ascii_gid[ch] : NO_GLYPH;
      if (g == NO_GLYPH) g = ascii_gid['?'];
      if (g == NO_GLYPH) continue;
      int x = col0 + c;
      int idx = row * L->cols + x;
      L->flags[idx] |= F_PENDING;
      L->msg[idx] = g;
      msg_cells[msg_n++] = idx;

      // Make sure a stream will come down this column soon.
      Stream *s = &L->streams[x];
      if (!s->active) {
        s->active = true;
        s->y      = -irand(1, row / 2 + 2);
        s->speed  = irand(MIN_SPEED, MAX_SPEED);
        s->tick   = 0;
        s->trail  = irand(MIN_TRAIL, MAX_TRAIL);
      }
    }
  }
  msg_state = msg_n ? MSG_REVEAL : MSG_IDLE;
  msg_timer = 0.0;
  msg_shown = true;
}

static void msg_update(double dt) {
  if (!g_msg) return;
  Layer *L = &layers[0];
  switch (msg_state) {
    case MSG_IDLE:
      msg_timer -= dt;
      if (msg_timer <= 0.0 && (!msg_shown || g_msg_every > 0.0f)) msg_start();
      break;
    case MSG_REVEAL:
      msg_timer += dt;
      if (msg_locked >= msg_n || msg_timer >= MSG_REVEAL_MAX) {
        for (int k = 0; k < msg_n; ++k) {         // pop in any stragglers
          int i = msg_cells[k];
          if (L->flags[i] & F_PENDING) {
            L->flags[i] = F_LOCKED;
            L->ch[i]    = L->msg[i];
            L->lvl[i]   = 1.0f;
          }
        }
        msg_locked = msg_n;
        msg_state  = MSG_HOLD;
        msg_timer  = 0.0;
      }
      break;
    case MSG_HOLD:
      msg_timer += dt;
      if (msg_timer >= MSG_HOLD_S) msg_release();
      break;
  }
}

// ===== Glyph atlas =====
static int next_p2(int x) {
  int p = 1; while (p < x) p <<= 1; return p;
}

static int utf8_encode(uint32_t cp, char out[5]) {
  if (cp < 0x80)    { out[0] = (char)cp; out[1] = 0; return 1; }
  if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6));
                      out[1] = (char)(0x80 | (cp & 0x3F)); out[2] = 0; return 2; }
  out[0] = (char)(0xE0 | (cp >> 12));
  out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[2] = (char)(0x80 | (cp & 0x3F));
  out[3] = 0;
  return 3;
}

static const char *g_font_path      = NULL;
static const char *g_kana_font_path = NULL;   // user-supplied (--kana-font)
static char       *kana_found_path  = NULL;   // cached result of the search
static bool        kana_searched    = false;
static int         g_font_pt        = DEFAULT_PT_SIZE;
static char       *used_font_path   = NULL;   // main font actually opened

static TTF_Font *try_kana_font(const char *path, int pt) {
  if (!path || !*path) return NULL;
  TTF_Font *f = TTF_OpenFont(path, pt);
  if (!f) return NULL;
  if (!TTF_GlyphIsProvided(f, 0xFF71)) { TTF_CloseFont(f); return NULL; }
  return f;
}

// Find a font with half-width katakana. The search runs once; later atlas
// rebuilds (HiDPI changes) reuse the path it found.
static TTF_Font *open_kana_font(int pt) {
  if (kana_searched) return kana_found_path ? try_kana_font(kana_found_path, pt) : NULL;
  kana_searched = true;

  const char *cands[] = {
    g_kana_font_path,
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc",
    "/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf",
    "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf",
    "/usr/share/fonts/truetype/takao-gothic/TakaoGothic.ttf",
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
    "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
    "/Library/Fonts/Arial Unicode.ttf",
  };
  for (size_t i = 0; i < sizeof cands / sizeof cands[0]; ++i) {
    TTF_Font *f = try_kana_font(cands[i], pt);
    if (f) { kana_found_path = strdup(cands[i]); return f; }
    if (i == 0 && g_kana_font_path)
      fprintf(stderr, "--kana-font %s has no half-width katakana; searching.\n",
              g_kana_font_path);
  }

#if !defined(__APPLE__) && !defined(_WIN32) && !defined(__EMSCRIPTEN__)
  // Ask fontconfig, if it's installed.
  FILE *fp = popen("fc-match -f '%{file}' ':charset=ff71' 2>/dev/null", "r");
  if (fp) {
    char buf[1024] = {0};
    if (fgets(buf, sizeof buf, fp)) {
      buf[strcspn(buf, "\r\n")] = 0;
      TTF_Font *f = try_kana_font(buf, pt);
      if (f) { pclose(fp); kana_found_path = strdup(buf); return f; }
    }
    pclose(fp);
  }
#endif
  return NULL;
}

static int load_font_build_atlas(const char *font_path, int pt_size) {
  TTF_Font *font = NULL;
  if (font_path) font = TTF_OpenFont(font_path, pt_size);

  if (!font) {
    if (font_path) fprintf(stderr, "Could not open font %s; trying defaults.\n", font_path);
    const char *cands[] = {
#ifdef __EMSCRIPTEN__
      "/fonts/NotoSansMonoCJKjp-Matrix.otf",   // bundled: ASCII + katakana
#endif
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
      "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
      "/System/Library/Fonts/SFNSMono.ttf",
      "/System/Library/Fonts/Menlo.ttc",
      "/Library/Fonts/Menlo.ttf",
      NULL
    };
    for (int i = 0; cands[i]; ++i) {
      font = TTF_OpenFont(cands[i], pt_size);
      if (font) {
        if (!atlas_tex) fprintf(stderr, "Using font: %s\n", cands[i]);
        free(used_font_path);
        used_font_path = strdup(cands[i]);
        break;
      }
    }
  } else {
    if (!atlas_tex) fprintf(stderr, "Using font: %s\n", font_path);
    free(used_font_path);
    used_font_path = strdup(font_path);
  }

  if (!font) {
    fprintf(stderr, "TTF_OpenFont failed: no usable font (try -F /path/to/font.ttf)\n");
    return -1;
  }
  TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
  TTF_SetFontKerning(font, 0);

  // Katakana: from the main font if it has them, else a CJK fallback.
  TTF_Font *kfont = NULL;
  bool      kfont_owned = false;
  if (TTF_GlyphIsProvided(font, 0xFF71)) {
    kfont = font;
  } else {
    bool first = !kana_searched;
    kfont = open_kana_font(pt_size);
    kfont_owned = (kfont != NULL);
    if (first) {
      if (kfont) fprintf(stderr, "Using katakana font: %s\n", kana_found_path);
      else       fprintf(stderr, "No font with half-width katakana found; using ASCII "
                                 "(install fonts-noto-cjk or pass --kana-font).\n");
    }
  }
  // CJK fonts draw half-width katakana smaller than Latin capitals at the same
  // point size; rescale so ｱ is about as tall as M.
  if (kfont && kfont != font && kana_found_path) {
    int a0, a1, a2, a3, b0, b1, b2, b3, adv;
    if (TTF_GlyphMetrics(font, 'M', &a0, &a1, &a2, &a3, &adv) == 0 &&
        TTF_GlyphMetrics(kfont, 0xFF71, &b0, &b1, &b2, &b3, &adv) == 0 &&
        a3 > a2 && b3 > b2) {
      float r = 1.05f * (float)(a3 - a2) / (float)(b3 - b2);
      if (r < 0.7f) r = 0.7f;
      if (r > 2.0f) r = 2.0f;
      if (fabsf(r - 1.0f) > 0.05f) {
        TTF_Font *k2 = try_kana_font(kana_found_path, (int)lroundf((float)pt_size * r));
        if (k2) { TTF_CloseFont(kfont); kfont = k2; }
      }
    }
  }
  if (kfont && kfont != font) TTF_SetFontHinting(kfont, TTF_HINTING_LIGHT);

  // Render every glyph first so the atlas cell can fit the largest one.
  SDL_Surface *surf[MAX_GLYPHS] = {0};
  uint32_t     cps[MAX_GLYPHS];
  bool         is_kana[MAX_GLYPHS];
  int          n = 0, max_w = 1, max_h = 1;

  for (uint32_t cp = 33; cp <= 126; ++cp)       { cps[n] = cp; is_kana[n] = false; ++n; }
  if (kfont)
    for (uint32_t cp = KANA_FIRST; cp <= KANA_LAST; ++cp) { cps[n] = cp; is_kana[n] = true; ++n; }

  SDL_Color white = { 255, 255, 255, 255 };
  for (int i = 0; i < n; ++i) {
    char text[5];
    utf8_encode(cps[i], text);
    SDL_Surface *gs = TTF_RenderUTF8_Blended(is_kana[i] ? kfont : font, text, white);
    if (!gs) continue;
    surf[i] = SDL_ConvertSurfaceFormat(gs, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(gs);
    if (!surf[i]) continue;
    if (surf[i]->w > max_w) max_w = surf[i]->w;
    if (surf[i]->h > max_h) max_h = surf[i]->h;
  }

  const int pad = 4;                 // room for mipmapping
  int cell_w = max_w + pad * 2, cell_h = max_h + pad * 2;
  int colsN  = 16;
  int rowsN  = (n + colsN - 1) / colsN;
  atlas_w = next_p2(colsN * cell_w);
  atlas_h = next_p2(rowsN * cell_h);

  SDL_Surface *atlas = SDL_CreateRGBSurfaceWithFormat(
      0, atlas_w, atlas_h, 32, SDL_PIXELFORMAT_ABGR8888);
  if (!atlas) {
    fprintf(stderr, "CreateRGBSurface: %s\n", SDL_GetError());
    for (int i = 0; i < n; ++i) if (surf[i]) SDL_FreeSurface(surf[i]);
    if (kfont_owned) TTF_CloseFont(kfont);
    TTF_CloseFont(font);
    return -1;
  }
  SDL_FillRect(atlas, NULL, SDL_MapRGBA(atlas->format, 0, 0, 0, 0));

  memset(glyphs, 0, sizeof glyphs);
  for (int i = 0; i < 128; ++i) ascii_gid[i] = NO_GLYPH;
  n_glyphs  = 0;
  have_kana = false;

  for (int i = 0; i < n; ++i) {
    if (!surf[i]) continue;
    int slot = n_glyphs;
    int gx = (slot % colsN) * cell_w + pad;
    int gy = (slot / colsN) * cell_h + pad;
    SDL_SetSurfaceBlendMode(surf[i], SDL_BLENDMODE_NONE);
    SDL_Rect dst = { gx, gy, surf[i]->w, surf[i]->h };
    SDL_BlitSurface(surf[i], NULL, atlas, &dst);

    Glyph *g = &glyphs[slot];
    g->u0   = (float)gx / (float)atlas_w;
    g->v0   = (float)gy / (float)atlas_h;
    g->u1   = (float)(gx + surf[i]->w) / (float)atlas_w;
    g->v1   = (float)(gy + surf[i]->h) / (float)atlas_h;
    g->w    = surf[i]->w;
    g->h    = surf[i]->h;
    g->cp   = cps[i];
    g->kana = is_kana[i];
    if (cps[i] < 128) ascii_gid[cps[i]] = (uint16_t)slot;
    if (is_kana[i])   have_kana = true;
    ++n_glyphs;
    SDL_FreeSurface(surf[i]);
  }

  if (atlas_tex == 0) glGenTextures(1, &atlas_tex);
  glBindTexture(GL_TEXTURE_2D, atlas_tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, atlas->pitch / 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, atlas_w, atlas_h, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, atlas->pixels);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glGenerateMipmap(GL_TEXTURE_2D);   // far layers draw glyphs scaled down
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  SDL_FreeSurface(atlas);
  if (kfont_owned) TTF_CloseFont(kfont);
  TTF_CloseFont(font);

  build_pools();
  return 0;
}

// ===== Shaders =====
#ifdef __EMSCRIPTEN__
static const char *GLSL_HEADER = "#version 300 es\nprecision highp float;\n";
#else
static const char *GLSL_HEADER = "#version 330 core\n";
#endif

// Glyph quads. aInfo = (level, head, halo, layer dim), all unorm8.
static const char *GLYPH_VS =
  "layout(location = 0) in vec2 aPos;\n"
  "layout(location = 1) in vec2 aUV;\n"
  "layout(location = 2) in vec4 aInfo;\n"
  "uniform vec2 uViewport;\n"
  "out vec2 vUV;\n"
  "out vec4 vInfo;\n"
  "void main() {\n"
  "  vUV = aUV;\n"
  "  vInfo = aInfo;\n"
  "  vec2 ndc = vec2(aPos.x / uViewport.x * 2.0 - 1.0,\n"
  "                  1.0 - aPos.y / uViewport.y * 2.0);\n"
  "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
  "}\n";

static const char *GLYPH_FS =
  "in vec2 vUV;\n"
  "in vec4 vInfo;\n"
  "out vec4 oColor;\n"
  "uniform sampler2D uAtlas;\n"
  "uniform float uHue;        // degrees\n"
  "uniform int   uMono;\n"
  "uniform int   uBloomPass;  // 1 = bright pass for bloom\n"
  "uniform float uThreshold;  // trail level where bloom starts\n"
  "vec3 hsv2rgb(float h, float s, float v) {\n"
  "  vec3 k = mod(vec3(5.0, 3.0, 1.0) + h / 60.0, 6.0);\n"
  "  return v - v * s * clamp(min(k, 4.0 - k), 0.0, 1.0);\n"
  "}\n"
  "void main() {\n"
  "  float cov   = texture(uAtlas, vUV).a;\n"
  "  float level = pow(vInfo.x, 0.45);  // perceptual: exp decay reads as linear\n"
  "  bool  head  = vInfo.y > 0.5;\n"
  "  float dim   = vInfo.w;             // layer depth: 1 front, less behind\n"
  "  vec3 rgb; float a;\n"
  "  if (uBloomPass == 1) {\n"
  "    a   = (head ? 1.0 : smoothstep(uThreshold, 1.0, level)) * dim * dim;\n"
  "    rgb = (uMono == 1) ? vec3(1.0) : hsv2rgb(uHue, 0.85, 1.0);\n"
  "  } else if (head) {\n"
  "    a   = dim;\n"
  "    rgb = (uMono == 1) ? vec3(0.95) : hsv2rgb(uHue, 0.05, 0.95);\n"
  "  } else {\n"
  "    // Freshly left cells are still white-hot and cool into the palette.\n"
  "    float heat = smoothstep(0.80, 1.0, level);\n"
  "    float v    = mix(0.25 + 0.75 * level, 0.97, heat);\n"
  "    a   = level * dim;\n"
  "    rgb = (uMono == 1) ? vec3(mix(0.15 + 0.85 * level, 0.95, heat))\n"
  "                       : hsv2rgb(uHue, mix(1.0, 0.2, heat), v);\n"
  "  }\n"
  "  if (vInfo.z > 0.5) a *= 0.25;  // soft halo quad (no-bloom mode)\n"
  "  oColor = vec4(rgb, a * cov);\n"
  "}\n";

// Fullscreen triangle from gl_VertexID; no vertex attributes.
static const char *FULLSCREEN_VS =
  "out vec2 vUV;\n"
  "void main() {\n"
  "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
  "  vUV = p;\n"
  "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
  "}\n";

static const char *BLUR_FS =
  "in vec2 vUV;\n"
  "out vec4 oColor;\n"
  "uniform sampler2D uTex;\n"
  "uniform vec2  uStep;      // one texel along the blur axis\n"
  "uniform int   uRadius;    // 1..5\n"
  "uniform float uW[6];      // normalized Gaussian weights\n"
  "void main() {\n"
  "  vec4 s = texture(uTex, vUV) * uW[0];\n"
  "  for (int i = 1; i <= 5; ++i) {\n"
  "    if (i > uRadius) break;\n"
  "    vec2 d = uStep * float(i);\n"
  "    s += (texture(uTex, vUV + d) + texture(uTex, vUV - d)) * uW[i];\n"
  "  }\n"
  "  oColor = s;\n"
  "}\n";

static const char *COMPOSITE_FS =
  "in vec2 vUV;\n"
  "out vec4 oColor;\n"
  "uniform sampler2D uTex;\n"
  "uniform float uIntensity;\n"
  "void main() {\n"
  "  oColor = vec4(texture(uTex, vUV).rgb * uIntensity, 1.0);\n"
  "}\n";

static GLuint compile_shader(GLenum type, const char *src, const char *name) {
  const char *srcs[2] = { GLSL_HEADER, src };
  GLuint s = glCreateShader(type);
  glShaderSource(s, 2, srcs, NULL);
  glCompileShader(s);
  GLint ok = GL_FALSE;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048]; GLsizei n = 0;
    glGetShaderInfoLog(s, (GLsizei)sizeof log, &n, log);
    fprintf(stderr, "Shader compile failed (%s):\n%.*s\n", name, (int)n, log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static GLuint link_program(const char *vs_src, const char *fs_src, const char *name) {
  GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src, name);
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src, name);
  if (!vs || !fs) {
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    return 0;
  }
  GLuint p = glCreateProgram();
  glAttachShader(p, vs);
  glAttachShader(p, fs);
  glLinkProgram(p);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = GL_FALSE;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048]; GLsizei n = 0;
    glGetProgramInfoLog(p, (GLsizei)sizeof log, &n, log);
    fprintf(stderr, "Program link failed (%s):\n%.*s\n", name, (int)n, log);
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

// ===== GPU state =====
typedef struct {
  GLuint prog;
  GLint  uViewport, uAtlas, uHue, uMono, uBloomPass, uThreshold;
} GlyphProg;

typedef struct {
  GLuint prog;
  GLint  uTex, uStep, uRadius, uW;
} BlurProg;

typedef struct {
  GLuint prog;
  GLint  uTex, uIntensity;
} CompositeProg;

static GlyphProg     P_glyph;
static BlurProg      P_blur;
static CompositeProg P_comp;

static GLuint glyph_vao = 0, glyph_vbo = 0, glyph_ibo = 0;
static GLuint empty_vao = 0;   // for attribute-less fullscreen draws

// Interleaved vertex: 20 bytes.
typedef struct {
  float   x, y;
  float   u, v;
  uint8_t level, head, halo, dim;
} Vertex;

static Vertex *verts      = NULL;
static size_t  verts_cap  = 0;   // in quads

static bool init_gpu(void) {
  P_glyph.prog = link_program(GLYPH_VS, GLYPH_FS, "glyph");
  P_blur.prog  = link_program(FULLSCREEN_VS, BLUR_FS, "blur");
  P_comp.prog  = link_program(FULLSCREEN_VS, COMPOSITE_FS, "composite");
  if (!P_glyph.prog || !P_blur.prog || !P_comp.prog) return false;

  P_glyph.uViewport  = glGetUniformLocation(P_glyph.prog, "uViewport");
  P_glyph.uAtlas     = glGetUniformLocation(P_glyph.prog, "uAtlas");
  P_glyph.uHue       = glGetUniformLocation(P_glyph.prog, "uHue");
  P_glyph.uMono      = glGetUniformLocation(P_glyph.prog, "uMono");
  P_glyph.uBloomPass = glGetUniformLocation(P_glyph.prog, "uBloomPass");
  P_glyph.uThreshold = glGetUniformLocation(P_glyph.prog, "uThreshold");

  P_blur.uTex    = glGetUniformLocation(P_blur.prog, "uTex");
  P_blur.uStep   = glGetUniformLocation(P_blur.prog, "uStep");
  P_blur.uRadius = glGetUniformLocation(P_blur.prog, "uRadius");
  P_blur.uW      = glGetUniformLocation(P_blur.prog, "uW");

  P_comp.uTex       = glGetUniformLocation(P_comp.prog, "uTex");
  P_comp.uIntensity = glGetUniformLocation(P_comp.prog, "uIntensity");

  glGenVertexArrays(1, &glyph_vao);
  glGenBuffers(1, &glyph_vbo);
  glGenBuffers(1, &glyph_ibo);
  glBindVertexArray(glyph_vao);
  glBindBuffer(GL_ARRAY_BUFFER, glyph_vbo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, glyph_ibo);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                        (void *)offsetof(Vertex, x));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                        (void *)offsetof(Vertex, u));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex),
                        (void *)offsetof(Vertex, level));
  glBindVertexArray(0);

  glGenVertexArrays(1, &empty_vao);

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  return true;
}

static void destroy_gpu(void) {
  if (P_glyph.prog) glDeleteProgram(P_glyph.prog);
  if (P_blur.prog)  glDeleteProgram(P_blur.prog);
  if (P_comp.prog)  glDeleteProgram(P_comp.prog);
  if (glyph_vbo)    glDeleteBuffers(1, &glyph_vbo);
  if (glyph_ibo)    glDeleteBuffers(1, &glyph_ibo);
  if (glyph_vao)    glDeleteVertexArrays(1, &glyph_vao);
  if (empty_vao)    glDeleteVertexArrays(1, &empty_vao);
  free(verts);
  verts = NULL; verts_cap = 0;
}

// Grow the CPU vertex array and the (static) index buffer to hold `quads`.
static bool reserve_quads(size_t quads) {
  if (quads <= verts_cap) return true;
  size_t cap = verts_cap ? verts_cap : 1024;
  while (cap < quads) cap *= 2;

  Vertex *nv = realloc(verts, cap * 4 * sizeof *nv);
  if (!nv) return false;
  verts = nv;

  GLuint *idx = malloc(cap * 6 * sizeof *idx);
  if (!idx) return false;
  for (size_t q = 0; q < cap; ++q) {
    GLuint b = (GLuint)(q * 4);
    GLuint *p = idx + q * 6;
    p[0] = b; p[1] = b + 1; p[2] = b + 2;
    p[3] = b; p[4] = b + 2; p[5] = b + 3;
  }
  glBindVertexArray(glyph_vao);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(cap * 6 * sizeof *idx),
               idx, GL_STATIC_DRAW);
  glBindVertexArray(0);
  free(idx);
  verts_cap = cap;
  return true;
}

static inline void put_quad(Vertex *v, float x0, float y0, float x1, float y1,
                            float u0, float v0, float u1, float v1,
                            uint8_t level, uint8_t head, uint8_t halo,
                            uint8_t dim) {
  v[0] = (Vertex){ x0, y0, u0, v0, level, head, halo, dim };
  v[1] = (Vertex){ x1, y0, u1, v0, level, head, halo, dim };
  v[2] = (Vertex){ x1, y1, u1, v1, level, head, halo, dim };
  v[3] = (Vertex){ x0, y1, u0, v1, level, head, halo, dim };
}

// Build all glyph quads for this frame (far layers first) in drawable-pixel
// coordinates and upload them in one go. Returns the number of quads.
static size_t build_glyph_batch(int view_w, int view_h, bool halos) {
  size_t max_quads = 0;
  for (int li = 0; li < g_layers; ++li)
    max_quads += (size_t)layers[li].rows * (size_t)layers[li].cols * (halos ? 2 : 1);
  if (!reserve_quads(max_quads)) return 0;

  size_t q = 0;
  for (int li = g_layers - 1; li >= 0; --li) {
    const Layer *L = &layers[li];
    if (!L->lvl) continue;
    const float cw = (float)view_w / (float)L->cols;
    const float ch = (float)view_h / (float)L->rows;
    const float s  = L->scale;
    const uint8_t D = (uint8_t)(L->dim * 255.0f + 0.5f);

    for (int y = 0; y < L->rows; ++y) {
      for (int x = 0; x < L->cols; ++x) {
        int   i   = y * L->cols + x;
        float lvl = L->lvl[i];
        if (lvl <= 0.0f) continue;

        uint16_t gid = L->ch[i];
        if (gid >= n_glyphs) continue;
        const Glyph *g = &glyphs[gid];
        if (g->w == 0) continue;

        // Center the glyph in its cell; snap the front layer to whole pixels.
        float gw = (float)g->w * s, gh = (float)g->h * s;
        float x0 = (float)x * cw + (cw - gw) * 0.5f;
        float y0 = (float)y * ch + (ch - gh) * 0.5f;
        if (li == 0) { x0 = floorf(x0 + 0.5f); y0 = floorf(y0 + 0.5f); }

        float u0 = g->u0, u1 = g->u1;
        if (L->flags[i] & F_MIRROR) { u0 = g->u1; u1 = g->u0; }

        uint8_t Lv = (uint8_t)(lvl * 255.0f + 0.5f);
        uint8_t H  = L->head[i] ? 255 : 0;

        if (halos && L->head[i]) {
          float ex = gw * 0.3f, ey = gh * 0.3f;   // 1.6x quad around the head
          put_quad(&verts[q * 4], x0 - ex, y0 - ey, x0 + gw + ex, y0 + gh + ey,
                   u0, g->v0, u1, g->v1, Lv, H, 255, D);
          ++q;
        }
        put_quad(&verts[q * 4], x0, y0, x0 + gw, y0 + gh,
                 u0, g->v0, u1, g->v1, Lv, H, 0, D);
        ++q;
      }
    }
  }

  glBindBuffer(GL_ARRAY_BUFFER, glyph_vbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(q * 4 * sizeof(Vertex)),
               q ? verts : NULL, GL_STREAM_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  return q;
}

// ===== Render targets (FBO ping-pong) =====
typedef struct { GLuint tex; GLuint fbo; int w, h; } RenderTarget;
static RenderTarget rtA = {0}, rtB = {0};
static bool  BLOOM_ON        = true;
static float BLOOM_INTENSITY = 1.0f;
static int   BLOOM_RADIUS    = 4;     // 1..5
static float BLOOM_SCALE     = 0.5f;  // downscale factor (0.25..1.0)
static float BLOOM_THRESHOLD = 0.6f;  // trail level where glow starts (0..0.95)
#define BLOOM_GAIN 2.5f   // head-only bright pass is sparse; scale so intensity 1.0 reads well

static void destroy_rt(RenderTarget *rt) {
  if (rt->tex)  { glDeleteTextures(1, &rt->tex);  rt->tex = 0; }
  if (rt->fbo)  { glDeleteFramebuffers(1, &rt->fbo); rt->fbo = 0; }
  rt->w = rt->h = 0;
}

static bool create_rt(RenderTarget *rt, int w, int h) {
  destroy_rt(rt);
  rt->w = w; rt->h = h;

  glGenTextures(1, &rt->tex);
  glBindTexture(GL_TEXTURE_2D, rt->tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  glGenFramebuffers(1, &rt->fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D, rt->tex, 0);
  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    fprintf(stderr, "FBO incomplete (0x%x)\n", (unsigned)status);
    destroy_rt(rt);
    return false;
  }
  return true;
}

static void ensure_bloom_rts(int win_w, int win_h) {
  int rw = (int)(win_w * BLOOM_SCALE);
  int rh = (int)(win_h * BLOOM_SCALE);
  if (rw < 8) rw = 8;
  if (rh < 8) rh = 8;
  if (!BLOOM_ON) return;
  bool ok = true;
  if (rtA.w != rw || rtA.h != rh) ok = create_rt(&rtA, rw, rh) && ok;
  if (rtB.w != rw || rtB.h != rh) ok = create_rt(&rtB, rw, rh) && ok;
  if (!ok) {
    fprintf(stderr, "Bloom disabled: could not create framebuffers.\n");
    destroy_rt(&rtA);
    destroy_rt(&rtB);
    BLOOM_ON = false;
  }
}

// ===== Drawing =====
static float current_hue(void) {
  return g_cycle ? g_hue : palette_base_hue(g_palette);
}

static void draw_glyphs(size_t quads, int view_w, int view_h, bool mono,
                        bool bloom_pass) {
  if (quads == 0) return;
  glUseProgram(P_glyph.prog);
  glUniform2f(P_glyph.uViewport, (float)view_w, (float)view_h);
  glUniform1i(P_glyph.uAtlas, 0);
  glUniform1f(P_glyph.uHue, current_hue());
  glUniform1i(P_glyph.uMono, mono ? 1 : 0);
  glUniform1i(P_glyph.uBloomPass, bloom_pass ? 1 : 0);
  glUniform1f(P_glyph.uThreshold, BLOOM_THRESHOLD);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, atlas_tex);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE);   // additive, alpha-weighted

  glBindVertexArray(glyph_vao);
  glDrawElements(GL_TRIANGLES, (GLsizei)(quads * 6), GL_UNSIGNED_INT, NULL);
  glBindVertexArray(0);
}

static void blur_pass(RenderTarget *dst, RenderTarget *src, bool horizontal) {
  float w[6] = {0};
  int   r = BLOOM_RADIUS < 1 ? 1 : (BLOOM_RADIUS > 5 ? 5 : BLOOM_RADIUS);
  float sigma = 0.5f * (float)r + 0.5f, sum = 0.0f;
  for (int i = 0; i <= r; ++i) {
    w[i] = expf(-(float)(i * i) / (2.0f * sigma * sigma));
    sum += (i == 0) ? w[i] : 2.0f * w[i];
  }
  for (int i = 0; i <= r; ++i) w[i] /= sum;

  glBindFramebuffer(GL_FRAMEBUFFER, dst->fbo);
  glViewport(0, 0, dst->w, dst->h);
  glDisable(GL_BLEND);

  glUseProgram(P_blur.prog);
  glUniform1i(P_blur.uTex, 0);
  glUniform2f(P_blur.uStep, horizontal ? 1.0f / (float)src->w : 0.0f,
                            horizontal ? 0.0f : 1.0f / (float)src->h);
  glUniform1i(P_blur.uRadius, r);
  glUniform1fv(P_blur.uW, 6, w);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, src->tex);
  glBindVertexArray(empty_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);
}

static void draw_frame(int win_w, int win_h, bool mono) {
  if (BLOOM_ON) ensure_bloom_rts(win_w, win_h);
  bool bloom = BLOOM_ON;

  size_t quads = build_glyph_batch(win_w, win_h, !bloom);

  if (bloom) {
    // 1) Bright pass (heads + fresh trail) into the downscaled buffer.
    glBindFramebuffer(GL_FRAMEBUFFER, rtA.fbo);
    glViewport(0, 0, rtA.w, rtA.h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_glyphs(quads, win_w, win_h, mono, true);

    // 2) Separable Gaussian: horizontal into B, vertical back into A.
    blur_pass(&rtB, &rtA, true);
    blur_pass(&rtA, &rtB, false);
  }

  // 3) Crisp glyphs to the backbuffer.
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, win_w, win_h);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  draw_glyphs(quads, win_w, win_h, mono, false);

  // 4) Add the blurred glow on top.
  if (bloom) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glUseProgram(P_comp.prog);
    glUniform1i(P_comp.uTex, 0);
    glUniform1f(P_comp.uIntensity, BLOOM_INTENSITY * BLOOM_GAIN);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, rtA.tex);
    glBindVertexArray(empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
  }
  glUseProgram(0);
}

static bool alloc_layers(int w, int h) {
  for (int i = 0; i < MAX_LAYERS; ++i)
    if (!alloc_layer(&layers[i], i, w, h)) { free_layers(); return false; }
  msg_reset(MSG_FIRST_DELAY);
  msg_shown = false;
  return true;
}

// ===== Help overlay =====
// A semi-transparent box with the key bindings and the current value of each
// setting. The text is rendered with SDL_ttf into a texture, re-rendered only
// when its content changes.
static const char *UI_VS =
  "layout(location = 0) in vec2 aPos;\n"
  "layout(location = 1) in vec2 aUV;\n"
  "uniform vec2 uViewport;\n"
  "out vec2 vUV;\n"
  "void main() {\n"
  "  vUV = aUV;\n"
  "  gl_Position = vec4(aPos.x / uViewport.x * 2.0 - 1.0,\n"
  "                     1.0 - aPos.y / uViewport.y * 2.0, 0.0, 1.0);\n"
  "}\n";

static const char *UI_FS =
  "in vec2 vUV;\n"
  "out vec4 oColor;\n"
  "uniform sampler2D uTex;\n"
  "uniform vec4 uColor;\n"
  "uniform int  uUseTex;\n"
  "void main() {\n"
  "  float a = (uUseTex == 1) ? texture(uTex, vUV).a : 1.0;\n"
  "  oColor = vec4(uColor.rgb, uColor.a * a);\n"
  "}\n";

typedef struct {
  int    sim_hz, density;
  bool   mono, paused;
} HelpState;

static bool      g_help      = false;
static GLuint    ui_prog     = 0, ui_vao = 0, ui_vbo = 0;
static GLint     ui_uViewport, ui_uTex, ui_uColor, ui_uUseTex;
static TTF_Font *help_font   = NULL;
static int       help_pt     = 0;
static GLuint    help_tex    = 0;
static int       help_w      = 0, help_h = 0;
static char      help_cache[4096];

static bool init_ui(void) {
  ui_prog = link_program(UI_VS, UI_FS, "ui");
  if (!ui_prog) return false;
  ui_uViewport = glGetUniformLocation(ui_prog, "uViewport");
  ui_uTex      = glGetUniformLocation(ui_prog, "uTex");
  ui_uColor    = glGetUniformLocation(ui_prog, "uColor");
  ui_uUseTex   = glGetUniformLocation(ui_prog, "uUseTex");

  glGenVertexArrays(1, &ui_vao);
  glGenBuffers(1, &ui_vbo);
  glBindVertexArray(ui_vao);
  glBindBuffer(GL_ARRAY_BUFFER, ui_vbo);
  glBufferData(GL_ARRAY_BUFFER, 16 * sizeof(float), NULL, GL_STREAM_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        (void *)(2 * sizeof(float)));
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  return true;
}

static void destroy_ui(void) {
  if (help_tex) glDeleteTextures(1, &help_tex);
  if (ui_vbo)   glDeleteBuffers(1, &ui_vbo);
  if (ui_vao)   glDeleteVertexArrays(1, &ui_vao);
  if (ui_prog)  glDeleteProgram(ui_prog);
  help_tex = ui_vbo = ui_vao = ui_prog = 0;
  if (help_font) { TTF_CloseFont(help_font); help_font = NULL; }
}

static void ui_rect(float x0, float y0, float x1, float y1, bool textured,
                    float r, float g, float b, float a) {
  const float v[16] = {
    x0, y0, 0.0f, 0.0f,   x1, y0, 1.0f, 0.0f,
    x0, y1, 0.0f, 1.0f,   x1, y1, 1.0f, 1.0f,
  };
  glBindBuffer(GL_ARRAY_BUFFER, ui_vbo);
  glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof v, v);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glUniform1i(ui_uUseTex, textured ? 1 : 0);
  glUniform4f(ui_uColor, r, g, b, a);
  glBindVertexArray(ui_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
}

#ifdef __EMSCRIPTEN__
#define QUIT_LINE       "                              "
#define FULLSCREEN_LINE "Enter  fullscreen (dbl-click) "
#else
#define QUIT_LINE       "q      quit                   "
#define FULLSCREEN_LINE "F11    fullscreen             "
#endif

static void build_help_text(char *out, size_t n, const HelpState *st) {
  char msg[48];
  if (g_msg) {
    size_t len = strlen(g_msg);
    snprintf(msg, sizeof msg, "\"%.24s%s\"", g_msg, len > 24 ? "..." : "");
  } else {
    snprintf(msg, sizeof msg, "(none; use -M)");
  }
  // Each value is shown in brackets, padded to a fixed column.
  char speed[24], dens[24], pause_[24], mono_[24], pal[24], cycon[24], cyc[24];
  char bloom[24], inten[24], rad[24], thr[24], lay[24], gly[24];
  snprintf(speed,  sizeof speed,  "[%d/s]", st->sim_hz);
  snprintf(dens,   sizeof dens,   "[%d%%]", st->density);
  snprintf(pause_, sizeof pause_, "[%s]", st->paused ? "on" : "off");
  snprintf(mono_,  sizeof mono_,  "[%s]", st->mono ? "on" : "off");
  snprintf(pal,    sizeof pal,    "[%s]", PALETTE_NAME[g_palette]);
  snprintf(cycon,  sizeof cycon,  "[%s]", g_cycle ? "on" : "off");
  snprintf(cyc,    sizeof cyc,    "[%.0f deg/s]", (double)g_cycle_speed);
  snprintf(bloom,  sizeof bloom,  "[%s]", BLOOM_ON ? "on" : "off");
  snprintf(inten,  sizeof inten,  "[%.1f]", (double)BLOOM_INTENSITY);
  snprintf(rad,    sizeof rad,    "[%d]", BLOOM_RADIUS);
  snprintf(thr,    sizeof thr,    "[%.2f]", (double)BLOOM_THRESHOLD);
  snprintf(lay,    sizeof lay,    "[%d]", g_layers);
  snprintf(gly,    sizeof gly,    "[%s]", have_kana ? GLYPH_MODE_NAME[g_glyph_mode] : "ascii");

  snprintf(out, n,
    "MATRIX RAIN                                  h / Esc: close\n"
    "\n"
    "%s p      pause        %s\n"
    "+ -    rain speed  %-12s[ ]    density      %s\n"
    "%s m      mono         %s\n"
    "\n"
    "n b    palette     %-12s1..7   quick palettes\n"
    "c      hue cycle   %-12s, .    cycle speed  %s\n"
    "\n"
    "v      bloom       %-12sg f    intensity    %s\n"
    "r e    blur radius %-12sy t    threshold    %s\n"
    "\n"
    "l      layers      %-12sk      glyphs       %s\n"
    "x      message     %s",
    QUIT_LINE, pause_,
    speed, dens,
    FULLSCREEN_LINE, mono_,
    pal,
    cycon, cyc,
    bloom, inten,
    rad, thr,
    lay, gly,
    msg);
}

static void draw_help(int win_w, int win_h, const HelpState *st) {
  if (!g_help || !ui_prog) return;

  // (Re)open the help font at a size that tracks the display scale.
  int pt = (int)lroundf(14.0f * (g_dpi_scale > 0.0f ? g_dpi_scale : 1.0f));
  if (!help_font || pt != help_pt) {
    if (help_font) TTF_CloseFont(help_font);
    help_font = used_font_path ? TTF_OpenFont(used_font_path, pt) : NULL;
    help_pt = pt;
    help_cache[0] = 0;
    if (!help_font) return;
  }

  char text[sizeof help_cache];
  build_help_text(text, sizeof text, st);
  if (strcmp(text, help_cache) != 0 || !help_tex) {
    // Render line by line (portable across SDL_ttf versions) into one surface.
    SDL_Color white = { 255, 255, 255, 255 };
    enum { MAX_HELP_LINES = 32 };
    SDL_Surface *ls[MAX_HELP_LINES] = {0};
    int nlines = 0, tw = 1, skip = TTF_FontLineSkip(help_font);
    char tmp[sizeof help_cache];
    memcpy(tmp, text, strlen(text) + 1);
    for (char *line = tmp, *nl; line && nlines < MAX_HELP_LINES; line = nl) {
      nl = strchr(line, '\n');
      if (nl) *nl++ = 0;
      if (*line) {
        SDL_Surface *ln = TTF_RenderUTF8_Blended(help_font, line, white);
        if (ln) {
          ls[nlines] = SDL_ConvertSurfaceFormat(ln, SDL_PIXELFORMAT_ABGR8888, 0);
          SDL_FreeSurface(ln);
          if (ls[nlines] && ls[nlines]->w > tw) tw = ls[nlines]->w;
        }
      }
      ++nlines;
    }
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, tw, nlines * skip, 32,
                                                    SDL_PIXELFORMAT_ABGR8888);
    if (s) SDL_FillRect(s, NULL, SDL_MapRGBA(s->format, 0, 0, 0, 0));
    for (int i = 0; i < nlines; ++i) {
      if (!ls[i]) continue;
      if (s) {
        SDL_SetSurfaceBlendMode(ls[i], SDL_BLENDMODE_NONE);
        SDL_Rect dst = { 0, i * skip, ls[i]->w, ls[i]->h };
        SDL_BlitSurface(ls[i], NULL, s, &dst);
      }
      SDL_FreeSurface(ls[i]);
    }
    if (!s) return;
    if (!help_tex) glGenTextures(1, &help_tex);
    glBindTexture(GL_TEXTURE_2D, help_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, s->pitch / 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s->w, s->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, s->pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    help_w = s->w; help_h = s->h;
    SDL_FreeSurface(s);
    memcpy(help_cache, text, strlen(text) + 1);
  }

  // Centre the box; shrink the text if the window is too small for it.
  float pad = 18.0f * (g_dpi_scale > 0.0f ? g_dpi_scale : 1.0f);
  float tw = (float)help_w, th = (float)help_h;
  float fit = fminf(1.0f, fminf(((float)win_w * 0.94f - 2.0f * pad) / tw,
                                ((float)win_h * 0.94f - 2.0f * pad) / th));
  if (fit < 0.1f) fit = 0.1f;
  tw *= fit; th *= fit;
  float x0 = floorf(((float)win_w - tw) * 0.5f), y0 = floorf(((float)win_h - th) * 0.5f);

  float tr, tg, tb;                         // light tint of the palette hue
  {
    float h = current_hue() / 60.0f, s = 0.25f, v = 1.0f;
    float c = v * s, x = c * (1.0f - fabsf(fmodf(h, 2.0f) - 1.0f)), m = v - c;
    int   i = (int)h % 6;
    float r1[6] = { c, x, 0, 0, x, c }, g1[6] = { x, c, c, x, 0, 0 },
          b1[6] = { 0, 0, x, c, c, x };
    tr = r1[i] + m; tg = g1[i] + m; tb = b1[i] + m;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, win_w, win_h);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glUseProgram(ui_prog);
  glUniform2f(ui_uViewport, (float)win_w, (float)win_h);
  glUniform1i(ui_uTex, 0);

  ui_rect(x0 - pad, y0 - pad, x0 + tw + pad, y0 + th + pad, false,
          0.0f, 0.0f, 0.0f, 0.78f);
  float bw = fmaxf(1.0f, g_dpi_scale);      // thin border
  ui_rect(x0 - pad, y0 - pad, x0 + tw + pad, y0 - pad + bw, false, tr, tg, tb, 0.5f);
  ui_rect(x0 - pad, y0 + th + pad - bw, x0 + tw + pad, y0 + th + pad, false, tr, tg, tb, 0.5f);
  ui_rect(x0 - pad, y0 - pad, x0 - pad + bw, y0 + th + pad, false, tr, tg, tb, 0.5f);
  ui_rect(x0 + tw + pad - bw, y0 - pad, x0 + tw + pad, y0 + th + pad, false, tr, tg, tb, 0.5f);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, help_tex);
  ui_rect(x0, y0, x0 + tw, y0 + th, true, tr, tg, tb, 1.0f);
  glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);
}

static void usage(const char *prog) {
  fprintf(stderr,
    "Usage: %s [options]\n"
    "  -f <steps/s>          rain speed (%d..%d, default %d)\n"
    "  -d <0..100>           stream density (default %d)\n"
    "  -s <pt>               cell size in points (default %d)\n"
    "  -P <pt>               font size in points (default %d)\n"
    "  -F <path>             font for Latin glyphs\n"
    "  -G mix|kana|ascii     glyph set (default mix: katakana + digits + symbols)\n"
    "  --kana-font <path>    font with half-width katakana (default: auto)\n"
    "  --no-mirror           don't mirror katakana\n"
    "  --mutate <0..20>      glyph swaps per lit cell per second (default 1.5)\n"
    "  --layers <1..3>       parallax depth layers (default 2)\n"
    "  -M <text>             message the rain freezes into (ASCII)\n"
    "  --message-every <s>   seconds between messages (default 15; 0 = once)\n"
    "  -m                    monochrome\n"
    "  -C <palette>          blue, green, purple, cyan, magenta, red, matrix\n"
    "  --cycle               hue cycling;  --cycle-speed <deg/s>\n"
    "  --no-bloom            disable bloom\n"
    "  --bloom-scale <0.25..1>  --bloom-radius <1..5>  --bloom-intensity <0..3>\n"
    "  --bloom-threshold <0..0.95>  trail level where glow starts (default 0.6)\n"
    "  --desktop             X11 desktop-wallpaper window (build with DESKTOP=1)\n"
    "Press h in the window for the key bindings.\n",
    prog, MIN_SIM_HZ, MAX_SIM_HZ, DEFAULT_SIM_HZ, DEFAULT_DENSITY,
    DEFAULT_CELL_PX, DEFAULT_PT_SIZE);
}

#if defined(ENABLE_X11_DESKTOP) && \
    (defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
     defined(__NetBSD__))
static void make_desktop_window(SDL_Window *win) {
  SDL_SysWMinfo info; SDL_VERSION(&info.version);
  if (SDL_GetWindowWMInfo(win, &info) && info.subsystem == SDL_SYSWM_X11) {
    Display *dpy = info.info.x11.display;
    Window   w   = info.info.x11.window;

    Atom type    = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom desktop = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    XChangeProperty(dpy, w, type, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&desktop, 1);

    Atom state        = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom below        = XInternAtom(dpy, "_NET_WM_STATE_BELOW", False);
    Atom sticky       = XInternAtom(dpy, "_NET_WM_STATE_STICKY", False);
    Atom skip_taskbar = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    Atom skip_pager   = XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False);
    Atom states[4]    = { below, sticky, skip_taskbar, skip_pager };

    XChangeProperty(dpy, w, state, XA_ATOM, 32, PropModeAppend,
                    (unsigned char *)states, 4);

    Atom net_wm_desktop = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
    unsigned long all   = 0xFFFFFFFF; // all workspaces
    XChangeProperty(dpy, w, net_wm_desktop, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&all, 1);

    XFlush(dpy);
  }
}
#endif

static PaletteId parse_palette(const char *s) {
  if (!s) return PAL_BLUE;
  if (!strcasecmp(s, "blue"))   return PAL_BLUE;
  if (!strcasecmp(s, "green"))  return PAL_GREEN;
  if (!strcasecmp(s, "purple")) return PAL_PURPLE;
  if (!strcasecmp(s, "cyan"))   return PAL_CYAN;
  if (!strcasecmp(s, "magenta"))return PAL_MAGENTA;
  if (!strcasecmp(s, "red"))    return PAL_RED;
  if (!strcasecmp(s, "matrix")) return PAL_MATRIX;
  return PAL_BLUE;
}

// ===== Layout & timing =====
// Recompute DPI scale, cell size, glyph atlas, grids and bloom targets
// whenever the drawable size or the window's pixel density changes. Polled
// every frame, so it also covers fullscreen toggles and moves between displays.
static bool update_layout(SDL_Window *win, int *last_dw, int *last_dh) {
  int ww, wh, dw, dh;
  SDL_GetWindowSize(win, &ww, &wh);
  SDL_GL_GetDrawableSize(win, &dw, &dh);
  if (dw <= 0 || dh <= 0) return true;  // minimized

  float scale = (ww > 0) ? (float)dw / (float)ww : 1.0f;
  if (scale < 1.0f) scale = 1.0f;
  bool scale_changed = fabsf(scale - g_dpi_scale) > 0.01f;

  if (!scale_changed && dw == *last_dw && dh == *last_dh) return true;

  if (scale_changed || atlas_tex == 0) {
    g_dpi_scale = scale;
    int pt = (int)lroundf((float)g_font_pt * scale);
    if (load_font_build_atlas(g_font_path, pt) != 0) {
      fprintf(stderr, "Font atlas build failed.\n");
      return false;
    }
  }

  cell_px = (int)lroundf((float)cell_pt * g_dpi_scale);
  if (cell_px < 4) cell_px = 4;
  if (!alloc_layers(dw, dh)) {
    fprintf(stderr, "Out of memory allocating the grid.\n");
    return false;
  }
  ensure_bloom_rts(dw, dh);

  *last_dw = dw;
  *last_dh = dh;
  return true;
}

#ifndef __EMSCRIPTEN__
// Seconds per refresh of the display the window is on (fallback 60 Hz).
static double display_period(SDL_Window *win) {
  SDL_DisplayMode m;
  int idx = SDL_GetWindowDisplayIndex(win);
  if (idx >= 0 && SDL_GetCurrentDisplayMode(idx, &m) == 0 && m.refresh_rate > 0)
    return 1.0 / (double)m.refresh_rate;
  return 1.0 / 60.0;
}
#endif

static inline double now_s(void) {
  return (double)SDL_GetPerformanceCounter() /
         (double)SDL_GetPerformanceFrequency();
}

static void set_palette(PaletteId p) {
  g_palette = p;
  if (!g_cycle) g_hue = palette_base_hue(g_palette);
}

static bool parse_glyph_mode(const char *s, GlyphMode *out) {
  for (int m = 0; m < GM_COUNT; ++m)
    if (!strcasecmp(s, GLYPH_MODE_NAME[m])) { *out = (GlyphMode)m; return true; }
  return false;
}

// ===== Application state & main loop =====
typedef struct {
  SDL_Window   *win;
  SDL_GLContext ctx;
  int           sim_hz, density;
  bool          mono, paused, fullscreen, running;
  double        sim_dt, prev_t;
  int           last_dw, last_dh;
} App;

static App app = {
  .sim_hz = DEFAULT_SIM_HZ, .density = DEFAULT_DENSITY, .running = true,
};

static void handle_key(const SDL_KeyboardEvent *ke) {
  SDL_Keycode k = ke->keysym.sym;
  if (k == SDLK_h || k == SDLK_F1 || k == SDLK_QUESTION ||
      (k == SDLK_SLASH && (ke->keysym.mod & KMOD_SHIFT))) { g_help = !g_help; return; }
  if (k == SDLK_ESCAPE) { g_help = false; return; }
#ifndef __EMSCRIPTEN__
  if (k == SDLK_q) { app.running = false; return; }
  if (k == SDLK_F11) {
    app.fullscreen = !app.fullscreen;
    SDL_SetWindowFullscreen(app.win, app.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    return;
  }
#endif
  switch (k) {
    case SDLK_p: app.paused = !app.paused; break;
    case SDLK_PLUS: case SDLK_EQUALS: case SDLK_KP_PLUS:
      app.sim_hz += 6; if (app.sim_hz > MAX_SIM_HZ) app.sim_hz = MAX_SIM_HZ;
      app.sim_dt = 1.0 / app.sim_hz;
      break;
    case SDLK_MINUS: case SDLK_KP_MINUS:
      app.sim_hz -= 6; if (app.sim_hz < MIN_SIM_HZ) app.sim_hz = MIN_SIM_HZ;
      app.sim_dt = 1.0 / app.sim_hz;
      break;
    case SDLK_LEFTBRACKET:  if (app.density > 0)   app.density -= 2; break;
    case SDLK_RIGHTBRACKET: if (app.density < 100) app.density += 2; break;
    case SDLK_m: app.mono = !app.mono; break;
    // Palette & cycling
    case SDLK_c: g_cycle = !g_cycle; if (!g_cycle) g_hue = palette_base_hue(g_palette); break;
    case SDLK_PERIOD: if (g_cycle_speed < 360) g_cycle_speed += 10.0f; break;
    case SDLK_COMMA:  if (g_cycle_speed > 0)   g_cycle_speed -= 10.0f; break;
    case SDLK_n: set_palette(next_palette(g_palette)); break;
    case SDLK_b: set_palette(prev_palette(g_palette)); break;
    case SDLK_1: set_palette(PAL_BLUE);    break;
    case SDLK_2: set_palette(PAL_GREEN);   break;
    case SDLK_3: set_palette(PAL_PURPLE);  break;
    case SDLK_4: set_palette(PAL_CYAN);    break;
    case SDLK_5: set_palette(PAL_MAGENTA); break;
    case SDLK_6: set_palette(PAL_RED);     break;
    case SDLK_7: set_palette(PAL_MATRIX);  break;
    // Bloom
    case SDLK_v: BLOOM_ON = !BLOOM_ON; break;
    case SDLK_g: BLOOM_INTENSITY += 0.1f; if (BLOOM_INTENSITY > 3.0f) BLOOM_INTENSITY = 3.0f; break;
    case SDLK_f: BLOOM_INTENSITY -= 0.1f; if (BLOOM_INTENSITY < 0.0f) BLOOM_INTENSITY = 0.0f; break;
    case SDLK_r: BLOOM_RADIUS += 1; if (BLOOM_RADIUS > 5) BLOOM_RADIUS = 5; break;
    case SDLK_e: BLOOM_RADIUS -= 1; if (BLOOM_RADIUS < 1) BLOOM_RADIUS = 1; break;
    case SDLK_y: BLOOM_THRESHOLD += 0.05f; if (BLOOM_THRESHOLD > 0.95f) BLOOM_THRESHOLD = 0.95f; break;
    case SDLK_t: BLOOM_THRESHOLD -= 0.05f; if (BLOOM_THRESHOLD < 0.0f)  BLOOM_THRESHOLD = 0.0f;  break;
    // Look
    case SDLK_l: g_layers = g_layers % MAX_LAYERS + 1; break;
    case SDLK_k: if (have_kana) g_glyph_mode = (GlyphMode)((g_glyph_mode + 1) % GM_COUNT); break;
    case SDLK_x: msg_start(); break;
    default: break;
  }
}

#ifdef __EMSCRIPTEN__
// Size of the browser viewport, read from a fixed full-page element in the
// shell page (#viewport), in CSS pixels.
static void web_viewport(int *vw, int *vh) {
  double w = 0.0, h = 0.0;
  if (emscripten_get_element_css_size("#viewport", &w, &h) != EMSCRIPTEN_RESULT_SUCCESS) w = h = 0.0;
  *vw = (int)w; *vh = (int)h;
}

// Keep the SDL window (and so the canvas) the size of the browser viewport.
static void web_sync_size(void) {
  int vw, vh;
  web_viewport(&vw, &vh);
  int w, h;
  SDL_GetWindowSize(app.win, &w, &h);
  if (vw > 0 && vh > 0 && (vw != w || vh != h)) SDL_SetWindowSize(app.win, vw, vh);
}
#endif

// One iteration of the main loop. Returns false to stop.
static bool frame(void) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT) app.running = false;
    else if (e.type == SDL_KEYDOWN) handle_key(&e.key);
  }
  if (!app.running) return false;

#ifdef __EMSCRIPTEN__
  web_sync_size();
#endif
  // Resize, fullscreen, DPI change, display move.
  if (!update_layout(app.win, &app.last_dw, &app.last_dh)) return false;

  // Real elapsed time; clamp so a stall (suspend, hidden tab) doesn't fast-forward.
  double t  = now_s();
  double dt = t - app.prev_t;
  app.prev_t = t;
  if (dt > 0.25) dt = 0.25;

  if (g_cycle) {
    g_hue += g_cycle_speed * (float)dt;
    if (g_hue >= 360.0f) g_hue = fmodf(g_hue, 360.0f);
  }

  if (!app.paused) {
    // Fades run on real time every frame; heads advance on fixed sim steps.
    // Each layer runs at its own speed.
    for (int li = 0; li < g_layers; ++li) {
      Layer *L = &layers[li];
      double step_dt = app.sim_dt / L->speed;   // real seconds per layer step
      fade_layer(L, dt, step_dt);
      L->acc += dt;
      int steps = 0;
      while (L->acc >= step_dt && steps < MAX_CATCHUP) {
        step_layer(L, app.density, step_dt);
        L->acc -= step_dt;
        ++steps;
      }
      if (steps == MAX_CATCHUP) L->acc = 0.0;  // can't keep up; drop backlog
    }
    msg_update(dt);
  }

  draw_frame(app.last_dw, app.last_dh, app.mono);
  if (g_help) {
    HelpState hs = { app.sim_hz, app.density, app.mono, app.paused };
    draw_help(app.last_dw, app.last_dh, &hs);
  }
  SDL_GL_SwapWindow(app.win);
  return true;
}

#ifdef __EMSCRIPTEN__
static void web_frame(void) {
  if (!frame()) emscripten_cancel_main_loop();
}
#endif

static void shutdown_app(void) {
  free_layers();
  free(msg_cells);
  free(kana_found_path);
  free(used_font_path);
  if (app.ctx) {
    if (atlas_tex) glDeleteTextures(1, &atlas_tex);
    destroy_rt(&rtA);
    destroy_rt(&rtB);
    destroy_gpu();
    destroy_ui();
    SDL_GL_DeleteContext(app.ctx);
  }
  if (app.win) SDL_DestroyWindow(app.win);
  TTF_Quit();
  SDL_Quit();
}

int main(int argc, char **argv) {
  bool want_desktop = false;

  for (int i = 1; i < argc; ++i) {
    if      (strcmp(argv[i], "-f") == 0 && i + 1 < argc) app.sim_hz = atoi(argv[++i]);
    else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
      app.density = atoi(argv[++i]);
      app.density = app.density < 0 ? 0 : (app.density > 100 ? 100 : app.density);
    }
    else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
      cell_pt = atoi(argv[++i]);
      if (cell_pt < 8)  cell_pt = 8;
      if (cell_pt > 64) cell_pt = 64;
    }
    else if (strcmp(argv[i], "-m") == 0) app.mono = true;
    else if (strcmp(argv[i], "-F") == 0 && i + 1 < argc) g_font_path = argv[++i];
    else if (strcmp(argv[i], "-P") == 0 && i + 1 < argc) {
      g_font_pt = atoi(argv[++i]);
      if (g_font_pt < 6)   g_font_pt = 6;
      if (g_font_pt > 128) g_font_pt = 128;
    }
    else if (strcmp(argv[i], "-G") == 0 && i + 1 < argc) {
      if (!parse_glyph_mode(argv[++i], &g_glyph_mode))
        fprintf(stderr, "Unknown glyph set '%s' (use mix, kana or ascii); using mix\n", argv[i]);
    }
    else if (strcmp(argv[i], "--kana-font") == 0 && i + 1 < argc) g_kana_font_path = argv[++i];
    else if (strcmp(argv[i], "--no-mirror") == 0) g_mirror = false;
    else if (strcmp(argv[i], "--mutate") == 0 && i + 1 < argc) {
      g_mutate = (float)atof(argv[++i]);
      if (g_mutate < 0.0f)  g_mutate = 0.0f;
      if (g_mutate > 20.0f) g_mutate = 20.0f;
    }
    else if (strcmp(argv[i], "--layers") == 0 && i + 1 < argc) {
      g_layers = atoi(argv[++i]);
      if (g_layers < 1) g_layers = 1;
      if (g_layers > MAX_LAYERS) g_layers = MAX_LAYERS;
    }
    else if (strcmp(argv[i], "-M") == 0 && i + 1 < argc) g_msg = argv[++i];
    else if (strcmp(argv[i], "--message-every") == 0 && i + 1 < argc) {
      g_msg_every = (float)atof(argv[++i]);
      if (g_msg_every < 0.0f) g_msg_every = 0.0f;
    }
    else if (strcmp(argv[i], "-C") == 0 && i + 1 < argc) {
      g_palette = parse_palette(argv[++i]);
      g_hue     = palette_base_hue(g_palette);
    }
    else if (strcmp(argv[i], "--cycle") == 0) g_cycle = true;
    else if (strcmp(argv[i], "--cycle-speed") == 0 && i + 1 < argc) {
      g_cycle_speed = (float)atof(argv[++i]);
      if (g_cycle_speed < 0)   g_cycle_speed = 0;
      if (g_cycle_speed > 360) g_cycle_speed = 360;
    }
    else if (strcmp(argv[i], "--desktop") == 0) want_desktop = true;
    else if (strcmp(argv[i], "--no-bloom") == 0) BLOOM_ON = false;
    else if (strcmp(argv[i], "--bloom-scale") == 0 && i + 1 < argc) {
      BLOOM_SCALE = (float)atof(argv[++i]);
      if (BLOOM_SCALE < 0.25f) BLOOM_SCALE = 0.25f;
      if (BLOOM_SCALE > 1.0f)  BLOOM_SCALE = 1.0f;
    }
    else if (strcmp(argv[i], "--bloom-radius") == 0 && i + 1 < argc) {
      BLOOM_RADIUS = atoi(argv[++i]);
      if (BLOOM_RADIUS < 1) BLOOM_RADIUS = 1;
      if (BLOOM_RADIUS > 5) BLOOM_RADIUS = 5;
    }
    else if (strcmp(argv[i], "--bloom-intensity") == 0 && i + 1 < argc) {
      BLOOM_INTENSITY = (float)atof(argv[++i]);
      if (BLOOM_INTENSITY < 0.0f) BLOOM_INTENSITY = 0.0f;
      if (BLOOM_INTENSITY > 3.0f) BLOOM_INTENSITY = 3.0f;
    }
    else if (strcmp(argv[i], "--bloom-threshold") == 0 && i + 1 < argc) {
      BLOOM_THRESHOLD = (float)atof(argv[++i]);
      if (BLOOM_THRESHOLD < 0.0f)  BLOOM_THRESHOLD = 0.0f;
      if (BLOOM_THRESHOLD > 0.95f) BLOOM_THRESHOLD = 0.95f;
    }
    else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      usage(argv[0]);
      return 0;
    }
  }
  if (app.sim_hz < MIN_SIM_HZ) app.sim_hz = MIN_SIM_HZ;
  if (app.sim_hz > MAX_SIM_HZ) app.sim_hz = MAX_SIM_HZ;

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  rng_state ^= (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ull ^ SDL_GetPerformanceCounter();
  if (!rng_state) rng_state = 1;

  if (TTF_Init() != 0) {
    fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
    SDL_Quit();
    return 1;
  }

#ifdef __EMSCRIPTEN__
  // WebGL2 = OpenGL ES 3.0
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  int win_w, win_h;
  web_viewport(&win_w, &win_h);
  if (win_w <= 0 || win_h <= 0) { win_w = 1280; win_h = 720; }
#else
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#ifdef __APPLE__
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
  int win_w = 1280, win_h = 720;
#endif
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE,   8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,  8);

  app.win = SDL_CreateWindow(
      "Matrix Rain (Bloom)",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      win_w, win_h,
      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!app.win) {
    fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    shutdown_app();
    return 1;
  }

  app.ctx = SDL_GL_CreateContext(app.win);
  if (!app.ctx) {
#ifdef __EMSCRIPTEN__
    fprintf(stderr, "SDL_GL_CreateContext: %s\nThis page needs WebGL2.\n", SDL_GetError());
#else
    fprintf(stderr, "SDL_GL_CreateContext: %s\n"
                    "This program needs OpenGL 3.3 (core profile).\n",
            SDL_GetError());
#endif
    shutdown_app();
    return 1;
  }

#ifndef __EMSCRIPTEN__
  // Prefer adaptive vsync, fall back to regular vsync. Either way the frame
  // limiter in the loop caps rendering at the refresh rate if vsync is ignored.
  if (SDL_GL_SetSwapInterval(-1) != 0) SDL_GL_SetSwapInterval(1);
#endif

  if (!init_gpu()) {
    fprintf(stderr, "GPU setup failed; exiting.\n");
    shutdown_app();
    return 1;
  }
  if (!init_ui()) fprintf(stderr, "Help overlay unavailable.\n");

#if defined(ENABLE_X11_DESKTOP) && \
    (defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
     defined(__NetBSD__))
  if (want_desktop) { make_desktop_window(app.win); SDL_SetWindowBordered(app.win, SDL_FALSE); }
#else
  if (want_desktop) {
    fprintf(stderr,
            "--desktop requested but X11 desktop support not compiled. "
            "Rebuild with: make DESKTOP=1\n");
  }
#endif

  if (!update_layout(app.win, &app.last_dw, &app.last_dh)) {
    shutdown_app();
    return 1;
  }
  if (g_glyph_mode != GM_ASCII && !have_kana) g_glyph_mode = GM_ASCII;

  app.sim_dt = 1.0 / app.sim_hz;
  app.prev_t = now_s();

#ifdef __EMSCRIPTEN__
  // The browser drives the loop (requestAnimationFrame); main() never returns.
  emscripten_set_main_loop(web_frame, 0, 1);
  return 0;
#else
  for (;;) {
    double frame_start = now_s();
    if (!frame()) break;
    // Frame limiter: with working vsync the swap already consumed the frame
    // and this does nothing; without vsync it caps at the display refresh rate.
    double remaining = display_period(app.win) - (now_s() - frame_start);
    if (remaining > 0.002) SDL_Delay((Uint32)((remaining - 0.001) * 1000.0));
  }
  int rc = app.running ? 1 : 0;   // still "running" means a fatal error
  shutdown_app();
  return rc;
#endif
}
