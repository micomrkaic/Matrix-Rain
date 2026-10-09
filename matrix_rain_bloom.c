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

// matrix_rain_bloom.c — SDL2 + OpenGL + SDL_ttf glyph atlas with real bloom
//
// Features
// - Additive "neon" bloom via render-to-texture + separable blur (ping‑pong FBOs)
// - Palettes (-C) + live hue cycling (--cycle)
// - Optional X11 desktop mode (compile with -DENABLE_X11_DESKTOP)
// - Low CPU: CPU updates grid; GPU does glow/blur/composite
//
// Linux build
//   sudo apt install libsdl2-dev libsdl2-ttf-dev
//   gcc -std=c11 -O2 -Wall -Wextra matrix_rain_bloom.c -o matrix_rain_bloom 
//     `sdl2-config --cflags --libs` -lSDL2_ttf -lGL -lm
//   # For --desktop support on X11
//   gcc -std=c11 -O2 -Wall -Wextra -DENABLE_X11_DESKTOP matrix_rain_bloom.c -o matrix_rain_bloom 
//     `sdl2-config --cflags --libs` -lSDL2_ttf -lGL -lm -lX11
//
// macOS build (Homebrew)
//   brew install sdl2 sdl2_ttf
//   clang -std=c11 -O2 -Wall -Wextra -I/opt/homebrew/include/SDL2 -L/opt/homebrew/lib 
//     matrix_rain_bloom.c -o matrix_rain_bloom -lSDL2 -lSDL2_ttf -framework OpenGL -lm
//
// Timing
//   -f sets the rain speed in simulation steps per second (default 60).
//   Rendering is paced by vsync / the display refresh rate, independently.
//   -s and -P are in logical points; they are scaled on HiDPI displays.
//
// Run examples
//   ./matrix_rain_bloom -f 60 -d 70 -s 18 -P 22 -C blue
//   ./matrix_rain_bloom --cycle --cycle-speed 25 --bloom-intensity 1.2
//   ./matrix_rain_bloom --bloom-scale 0.5 --bloom-radius 4
//   ./matrix_rain_bloom --desktop -C matrix   # requires -DENABLE_X11_DESKTOP + -lX11
//
// Keys
//   q quit • p pause • + / - speed • [ / ] density • m mono • F11 fullscreen
//   c hue cycling • , / . cycle speed −/+ • n / b palettes • 1..7 quick palettes
//   v toggle bloom • g / f bloom intensity +/− • r / e blur radius +/−

#define _XOPEN_SOURCE 700

#include <SDL.h>
#include <SDL_ttf.h>

#ifdef __APPLE__
  #include <OpenGL/gl.h>
#else
  #define GL_GLEXT_PROTOTYPES 1
  #include <GL/gl.h>
  #include <GL/glext.h>
#endif

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static inline int irand(int lo, int hi) {
  return lo + (int)((double)(hi - lo + 1) * (rand() / (RAND_MAX + 1.0)));
}

static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Character pool
//static const char *GLYPH_POOL =    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz@#$%^&*()[]{}<>|/\;:,.+=-_`~'\"";
static const char *GLYPH_POOL = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz@#$%^&*()[]{}<>|/\\;:,.+=-_`~'\"";


// Grid & streams
static unsigned char *grid_age = NULL; // rows*cols
static unsigned int  *grid_ch  = NULL; // UTF-32 (we use ASCII mostly)
static int G_ROWS = 0, G_COLS = 0;
static int   cell_pt     = DEFAULT_CELL_PX; // logical size (user, -s)
static int   cell_px     = DEFAULT_CELL_PX; // physical size = cell_pt * dpi
static float g_dpi_scale = 0.0f;            // drawable px per window pt

typedef struct {
  bool active;
  int  y;
  int  speed;
  int  tick;
  int  trail;
} Stream;

static Stream *cols = NULL;

// Font atlas
typedef struct {
  float u0, v0, u1, v1;
  int   w, h;
  int   advance;
  int   bearingY;
} Glyph;

static GLuint atlas_tex = 0;
static int    atlas_w = 0, atlas_h = 0;
static Glyph  glyphs[128];
static int    line_ascent = 0, line_descent = 0, line_height = 0;

// ===== Palette / color logic =====
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

static PaletteId next_palette(PaletteId p) {
  return (PaletteId)((((int)p) + 1) % PAL_COUNT);
}

static PaletteId prev_palette(PaletteId p) {
  return (PaletteId)((((int)p) + PAL_COUNT - 1) % PAL_COUNT);
}

static void hsv_to_rgb(float h, float s, float v,
                       float *r, float *g, float *b) {
  float c  = v * s;
  float hh = fmodf(h, 360.0f) / 60.0f;
  if (hh < 0) hh += 6.0f;
  float x  = c * (1.0f - fabsf(fmodf(hh, 2.0f) - 1.0f));

  float r1 = 0, g1 = 0, b1 = 0;
  int   hi = (int)hh;
  switch (hi) {
    case 0: r1 = c; g1 = x; b1 = 0; break;
    case 1: r1 = x; g1 = c; b1 = 0; break;
    case 2: r1 = 0; g1 = c; b1 = x; break;
    case 3: r1 = 0; g1 = x; b1 = c; break;
    case 4: r1 = x; g1 = 0; b1 = c; break;
    default: r1 = c; g1 = 0; b1 = x; break;
  }
  float m = v - c;
  *r = r1 + m; *g = g1 + m; *b = b1 + m;
}

static void color_for_cell(unsigned char age, bool mono,
                           float *R, float *G, float *B) {
  float t       = (float)age / (float)MAX_TRAIL;
  bool  is_head = (age == MAX_TRAIL);

  if (mono) {
    float m = is_head ? 0.95f : (0.15f + t * 0.85f);
    *R = *G = *B = m;
    return;
  }

  float hue = g_cycle ? g_hue : palette_base_hue(g_palette);
  float sat = is_head ? 0.05f : 1.0f;
  float val = is_head ? 0.95f : clampf(0.25f + t * 0.75f, 0.15f, 1.0f);
  hsv_to_rgb(hue, sat, val, R, G, B);
}

// ===== Grid & streams =====
static void free_grid(void) {
  free(grid_age); grid_age = NULL;
  free(grid_ch);  grid_ch  = NULL;
  free(cols);     cols     = NULL;
}

static void alloc_grid(int rows, int ncols) {
  free_grid();
  G_ROWS = rows > 1 ? rows : 1;
  G_COLS = ncols > 1 ? ncols : 1;

  grid_age = (unsigned char *)calloc((size_t)G_ROWS * (size_t)G_COLS,
                                     sizeof(unsigned char));
  grid_ch  = (unsigned int  *)calloc((size_t)G_ROWS * (size_t)G_COLS,
                                     sizeof(unsigned int));
  cols     = (Stream *)calloc((size_t)G_COLS, sizeof(Stream));

  for (int x = 0; x < G_COLS; ++x) {
    cols[x].active = false;
    cols[x].y      = -irand(1, G_ROWS);
    cols[x].speed  = irand(MIN_SPEED, MAX_SPEED);
    cols[x].tick   = 0;
    cols[x].trail  = irand(MIN_TRAIL, MAX_TRAIL);
  }
}

static void decay_cells(void) {
  int N = G_ROWS * G_COLS;
  for (int i = 0; i < N; ++i) if (grid_age[i] > 0) grid_age[i]--;
}

static inline unsigned int rand_char(void) {
  size_t n = strlen(GLYPH_POOL);
  return (unsigned int)GLYPH_POOL[irand(0, (int)n - 1)];
}

static void step_streams(int density_pct) {
  for (int x = 0; x < G_COLS; ++x) {
    Stream *s = &cols[x];

    if (!s->active) {
      int prob = (START_PROB_PCT * density_pct) / 100;
      if (irand(1, 100) <= prob) {
        s->active = true;
        s->y      = -irand(1, G_ROWS / 2);
        s->speed  = irand(MIN_SPEED, MAX_SPEED);
        s->tick   = 0;
        s->trail  = irand(MIN_TRAIL, MAX_TRAIL);
      }
      continue;
    }

    if (++s->tick < s->speed) continue;
    s->tick = 0;

    if (s->y >= 0 && s->y < G_ROWS) {
      unsigned char *prev = &grid_age[s->y * G_COLS + x];
      if (*prev < (unsigned char)s->trail) *prev = (unsigned char)s->trail;
    }

    s->y++;
    if (s->y >= G_ROWS + irand(1, G_ROWS / 3 + 1)) {
      s->active = false;
      continue;
    }

    if (s->y >= 0 && s->y < G_ROWS) {
      int idx = s->y * G_COLS + x;
      grid_age[idx] = MAX_TRAIL;
      grid_ch[idx]  = rand_char();
    }
  }
}

// ---- Glyph atlas build ----
static int next_p2(int x) {
  int p = 1; while (p < x) p <<= 1; return p;
}

static int load_font_build_atlas(const char *font_path, int pt_size) {
  TTF_Font *font = NULL;
  if (font_path) font = TTF_OpenFont(font_path, pt_size);

  if (!font) {
    const char *cands[] = {
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
      "/System/Library/Fonts/SFNSMono.ttf",
      "/System/Library/Fonts/Menlo.ttc",
      "/Library/Fonts/Menlo.ttf",
      NULL
    };
    for (int i = 0; cands[i]; ++i) {
      font = TTF_OpenFont(cands[i], pt_size);
      if (font) { fprintf(stderr, "Using font: %s\n", cands[i]); break; }
    }
  } else {
    fprintf(stderr, "Using font: %s\n", font_path);
  }

  if (!font) {
    fprintf(stderr, "TTF_OpenFont failed: no usable font (try -F /path/to/font.ttf)\n");
    return -1;
  }

  TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
  TTF_SetFontKerning(font, 0);
  line_ascent  = TTF_FontAscent(font);
  line_descent = TTF_FontDescent(font);
  line_height  = TTF_FontHeight(font);

  int first = 32, last = 126, count = last - first + 1;
  int pad = 2;
  int colsN = 16;
  int rowsN = (count + colsN - 1) / colsN;

  int cell_w, cell_h;
  {
    int minx, maxx, miny, maxy, advance;
    TTF_GlyphMetrics(font, 'M', &minx, &maxx, &miny, &maxy, &advance);
    cell_w = (maxx - minx) + pad * 2; if (cell_w < 8) cell_w = 8;
    cell_h = line_height + pad * 2;   if (cell_h < 8) cell_h = 8;
  }

  atlas_w = next_p2(colsN * cell_w);
  atlas_h = next_p2(rowsN * cell_h);

  SDL_Surface *atlas = SDL_CreateRGBSurfaceWithFormat(
      0, atlas_w, atlas_h, 32, SDL_PIXELFORMAT_ABGR8888);
  if (!atlas) {
    fprintf(stderr, "CreateRGBSurface: %s\n", SDL_GetError());
    TTF_CloseFont(font);
    return -1;
  }
  SDL_FillRect(atlas, NULL, SDL_MapRGBA(atlas->format, 0, 0, 0, 0));

  memset(glyphs, 0, sizeof glyphs);
  int gi = 0;
  for (int c = first; c <= last; ++c) {
    char       text[2]  = { (char)c, 0 };
    SDL_Color  white    = { 255, 255, 255, 255 };
    SDL_Surface *gs     = TTF_RenderUTF8_Blended(font, text, white);
    if (!gs) continue;
    SDL_Surface *rgba   = SDL_ConvertSurfaceFormat(gs, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(gs);
    if (!rgba) continue;

    int gx = (gi % colsN) * cell_w + pad;
    int gy = (gi / colsN) * cell_h + pad + (cell_h - line_height) / 2;

    SDL_Rect dst = { gx, gy, rgba->w, rgba->h };
    SDL_BlitSurface(rgba, NULL, atlas, &dst);

    Glyph *g = &glyphs[c];
    g->u0 = (float)gx / (float)atlas_w;
    g->v0 = (float)gy / (float)atlas_h;
    g->u1 = (float)(gx + rgba->w) / (float)atlas_w;
    g->v1 = (float)(gy + rgba->h) / (float)atlas_h;
    g->w  = rgba->w;
    g->h  = rgba->h;
    {
      int minx, maxx, miny, maxy, advance;
      TTF_GlyphMetrics(font, c, &minx, &maxx, &miny, &maxy, &advance);
      g->advance  = advance;
      g->bearingY = maxy;
    }

    SDL_FreeSurface(rgba);
    ++gi;
  }

  if (atlas_tex == 0) { glGenTextures(1, &atlas_tex); }
  glBindTexture(GL_TEXTURE_2D, atlas_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, atlas_w, atlas_h, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, atlas->pixels);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
  glBindTexture(GL_TEXTURE_2D, 0);

  SDL_FreeSurface(atlas);
  TTF_CloseFont(font);
  return 0;
}

// ===== Render targets (FBO ping‑pong) =====
typedef struct { GLuint tex; GLuint fbo; int w, h; } RenderTarget;
static RenderTarget rtA = {0}, rtB = {0};
static bool  BLOOM_ON        = true;
static float BLOOM_INTENSITY = 1.0f;
static int   BLOOM_RADIUS    = 4;     // 1..5
static float BLOOM_SCALE     = 0.5f;  // downscale factor (0.25..1.0)

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
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
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
static void set_ortho(int w, int h) {
  glViewport(0, 0, w, h);
  glMatrixMode(GL_PROJECTION); glLoadIdentity();
  glOrtho(0, w, h, 0, -1, 1);
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
}

static void draw_glyphs_layer(int view_w, int view_h,
                              bool mono, bool crisp_only) {
  // Draw glyphs into the CURRENT framebuffer at given viewport size.
  set_ortho(view_w, view_h);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE);
  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, atlas_tex);

  for (int y = 0; y < G_ROWS; ++y) {
    for (int x = 0; x < G_COLS; ++x) {
      unsigned char a = grid_age[y * G_COLS + x];
      if (a == 0) continue;

      unsigned int cp = grid_ch[y * G_COLS + x];
      if (cp < 32 || cp > 126) cp = '*';
      Glyph *g = &glyphs[cp];

      float bx = (float)(x * cell_px) * (float)view_w / (float)(G_COLS * cell_px);
      float by = (float)(y * cell_px) * (float)view_h / (float)(G_ROWS * cell_px);

      float r, gc, b; color_for_cell(a, mono, &r, &gc, &b);
      float t     = (float)a / (float)MAX_TRAIL;
      float alpha = clampf(0.15f + t * 0.85f, 0.15f, 1.0f);

      float gw = (float)g->w * (float)view_w / (float)(G_COLS * cell_px);
      float gh = (float)g->h * (float)view_h / (float)(G_ROWS * cell_px);
      float cx = bx + ((float)view_w / G_COLS - gw) * 0.5f;
      float cy = by + ((float)view_h / G_ROWS - gh) * 0.5f;

      int passes = crisp_only ? 1 : (a == MAX_TRAIL ? 2 : 1);
      for (int p = passes - 1; p >= 0; --p) {
        float scale      = (p == 0 ? 1.0f : 1.6f);
        float a2         = alpha * (p == 0 ? 1.0f : 0.25f);
        float half_extra = (scale - 1.0f) * 0.5f;
        float x0 = cx - gw * half_extra, y0 = cy - gh * half_extra;
        float x1 = x0 + gw * scale,      y1 = y0 + gh * scale;

        glColor4f(r, gc, b, a2);
        glBegin(GL_QUADS);
          glTexCoord2f(g->u0, g->v0); glVertex2f(x0, y0);
          glTexCoord2f(g->u1, g->v0); glVertex2f(x1, y0);
          glTexCoord2f(g->u1, g->v1); glVertex2f(x1, y1);
          glTexCoord2f(g->u0, g->v1); glVertex2f(x0, y1);
        glEnd();
      }
    }
  }

  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_TEXTURE_2D);
}

static void blur_pass(RenderTarget *dst, RenderTarget *src,
                      int radius, bool horizontal) {
  glBindFramebuffer(GL_FRAMEBUFFER, dst->fbo);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);

  set_ortho(dst->w, dst->h);
  glEnable(GL_BLEND);         // additive accumulation
  glBlendFunc(GL_ONE, GL_ONE);
  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, src->tex);

  // Simple Gaussian-ish kernel weights for radius up to 5; mirror-symmetric
  const float base_w[6] = { 0.227027f, 0.1945946f, 0.1216216f, 0.054054f, 0.016216f, 0.0035f };
  int r = radius; if (r > 5) r = 5; if (r < 1) r = 1;

  // center sample
  glColor4f(base_w[0], base_w[0], base_w[0], base_w[0]);
  glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 0);
    glTexCoord2f(1, 0); glVertex2f(dst->w, 0);
    glTexCoord2f(1, 1); glVertex2f(dst->w, dst->h);
    glTexCoord2f(0, 1); glVertex2f(0, dst->h);
  glEnd();

  // offset samples
  for (int i = 1; i <= r; ++i) {
    float w  = base_w[i];
    float dx = horizontal ? (float)i / (float)dst->w : 0.0f;
    float dy = horizontal ? 0.0f : (float)i / (float)dst->h;

    glColor4f(w, w, w, w);
    // positive offset
    glBegin(GL_QUADS);
      glTexCoord2f(0 + dx, 0 + dy); glVertex2f(0, 0);
      glTexCoord2f(1 + dx, 0 + dy); glVertex2f(dst->w, 0);
      glTexCoord2f(1 + dx, 1 + dy); glVertex2f(dst->w, dst->h);
      glTexCoord2f(0 + dx, 1 + dy); glVertex2f(0, dst->h);
    glEnd();

    // negative offset
    glBegin(GL_QUADS);
      glTexCoord2f(0 - dx, 0 - dy); glVertex2f(0, 0);
      glTexCoord2f(1 - dx, 0 - dy); glVertex2f(dst->w, 0);
      glTexCoord2f(1 - dx, 1 - dy); glVertex2f(dst->w, dst->h);
      glTexCoord2f(0 - dx, 1 - dy); glVertex2f(0, dst->h);
    glEnd();
  }

  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_TEXTURE_2D);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void draw_frame_no_bloom(int win_w, int win_h, bool mono);

static void draw_frame_with_bloom(int win_w, int win_h, bool mono) {
  ensure_bloom_rts(win_w, win_h);
  if (!BLOOM_ON) { draw_frame_no_bloom(win_w, win_h, mono); return; }

  // 1) Draw bright layer into rtA (downscaled)
  glBindFramebuffer(GL_FRAMEBUFFER, rtA.fbo);
  glViewport(0, 0, rtA.w, rtA.h);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  draw_glyphs_layer(rtA.w, rtA.h, mono, true /* crisp only; no fake glow */);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // 2) Blur horizontally into rtB, then vertically back into rtA
  blur_pass(&rtB, &rtA, BLOOM_RADIUS, true);
  blur_pass(&rtA, &rtB, BLOOM_RADIUS, false);

  // 3) Draw crisp base to backbuffer
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  draw_glyphs_layer(win_w, win_h, mono, true);

  // 4) Composite bloom additively at full-res
  glDisable(GL_DEPTH_TEST);
  glBlendFunc(GL_ONE, GL_ONE); // additive

  set_ortho(win_w, win_h);
  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, rtA.tex);
  glColor4f(BLOOM_INTENSITY, BLOOM_INTENSITY, BLOOM_INTENSITY, BLOOM_INTENSITY);

  glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 0);
    glTexCoord2f(1, 0); glVertex2f(win_w, 0);
    glTexCoord2f(1, 1); glVertex2f(win_w, win_h);
    glTexCoord2f(0, 1); glVertex2f(0, win_h);
  glEnd();

  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_TEXTURE_2D);
}

static void draw_frame_no_bloom(int win_w, int win_h, bool mono) {
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  draw_glyphs_layer(win_w, win_h, mono, false /* keep per-glyph glow */);
}

static void compute_grid_from_window(int w, int h) {
  int rows = h / cell_px; if (rows < 2) rows = 2;
  int cols = w / cell_px; if (cols < 2) cols = 2;
  alloc_grid(rows, cols);
}

static void usage(const char *prog) {
  fprintf(stderr,
    "Usage: %s [-f steps/s] [-d density0-100] [-s cell_pt] [-m] [-F fontpath] [-P pt]\n"
    "       [-C palette] [--cycle] [--cycle-speed degps] [--desktop] [--no-bloom]\n"
    "       [--bloom-scale s] [--bloom-radius r] [--bloom-intensity k]\n"
    "  -f  rain speed in simulation steps/s (%d..%d, default %d)\n"
    "Palettes: blue, green, purple, cyan, magenta, red, matrix\n",
    prog, MIN_SIM_HZ, MAX_SIM_HZ, DEFAULT_SIM_HZ);
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
static const char *g_font_path = NULL;
static int         g_font_pt   = DEFAULT_PT_SIZE;

// Recompute DPI scale, cell size, glyph atlas, grid and bloom targets whenever
// the drawable size or the window's pixel density changes. Polled every frame,
// so it also covers fullscreen toggles and moves between displays.
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
    if (load_font_build_atlas(g_font_path, pt) != 0) return false;
  }

  cell_px = (int)lroundf((float)cell_pt * g_dpi_scale);
  if (cell_px < 4) cell_px = 4;
  compute_grid_from_window(dw, dh);
  ensure_bloom_rts(dw, dh);

  *last_dw = dw;
  *last_dh = dh;
  return true;
}

// Seconds per refresh of the display the window is on (fallback 60 Hz).
static double display_period(SDL_Window *win) {
  SDL_DisplayMode m;
  int idx = SDL_GetWindowDisplayIndex(win);
  if (idx >= 0 && SDL_GetCurrentDisplayMode(idx, &m) == 0 && m.refresh_rate > 0)
    return 1.0 / (double)m.refresh_rate;
  return 1.0 / 60.0;
}

static inline double now_s(void) {
  return (double)SDL_GetPerformanceCounter() /
         (double)SDL_GetPerformanceFrequency();
}

static void set_palette(PaletteId p) {
  g_palette = p;
  if (!g_cycle) g_hue = palette_base_hue(g_palette);
}

int main(int argc, char **argv) {
  int  sim_hz = DEFAULT_SIM_HZ, density = DEFAULT_DENSITY;
  bool mono = false, want_desktop = false;

  for (int i = 1; i < argc; ++i) {
    if      (strcmp(argv[i], "-f") == 0 && i + 1 < argc) sim_hz = atoi(argv[++i]);
    else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
      density = atoi(argv[++i]);
      density = density < 0 ? 0 : (density > 100 ? 100 : density);
    }
    else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
      cell_pt = atoi(argv[++i]);
      if (cell_pt < 8)  cell_pt = 8;
      if (cell_pt > 64) cell_pt = 64;
    }
    else if (strcmp(argv[i], "-m") == 0) mono = true;
    else if (strcmp(argv[i], "-F") == 0 && i + 1 < argc) g_font_path = argv[++i];
    else if (strcmp(argv[i], "-P") == 0 && i + 1 < argc) {
      g_font_pt = atoi(argv[++i]);
      if (g_font_pt < 6)   g_font_pt = 6;
      if (g_font_pt > 128) g_font_pt = 128;
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
    else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      usage(argv[0]);
      return 0;
    }
  }
  if (sim_hz < MIN_SIM_HZ) sim_hz = MIN_SIM_HZ;
  if (sim_hz > MAX_SIM_HZ) sim_hz = MAX_SIM_HZ;

  srand((unsigned)time(NULL));
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  if (TTF_Init() != 0) {
    fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
    SDL_Quit();
    return 1;
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                      SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE,   8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,  8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

  int           rc  = 1;
  SDL_Window   *win = NULL;
  SDL_GLContext ctx = NULL;

  win = SDL_CreateWindow(
      "Matrix Rain (Bloom)",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      1280, 720,
      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!win) {
    fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    goto cleanup;
  }

  ctx = SDL_GL_CreateContext(win);
  if (!ctx) {
    fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
    goto cleanup;
  }

  // Prefer adaptive vsync, fall back to regular vsync. Either way the frame
  // limiter below caps rendering at the refresh rate if vsync is ignored.
  if (SDL_GL_SetSwapInterval(-1) != 0) SDL_GL_SetSwapInterval(1);

#if defined(ENABLE_X11_DESKTOP) && \
    (defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
     defined(__NetBSD__))
  if (want_desktop) { make_desktop_window(win); SDL_SetWindowBordered(win, SDL_FALSE); }
#else
  if (want_desktop) {
    fprintf(stderr,
            "--desktop requested but X11 desktop support not compiled. "
            "Rebuild with: make DESKTOP=1\n");
  }
#endif

  int last_dw = 0, last_dh = 0;
  if (!update_layout(win, &last_dw, &last_dh)) {
    fprintf(stderr, "Font atlas build failed; exiting.\n");
    goto cleanup;
  }

  bool   running = true, paused = false, fullscreen = false;
  double sim_dt  = 1.0 / sim_hz;
  double acc     = 0.0;
  double prev_t  = now_s();

  while (running) {
    double frame_start = now_s();

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) {
        running = false;
      } else if (e.type == SDL_KEYDOWN) {
        SDL_Keycode k = e.key.keysym.sym;
        if      (k == SDLK_q) running = false;
        else if (k == SDLK_p) paused = !paused;
        else if (k == SDLK_PLUS || k == SDLK_EQUALS || k == SDLK_KP_PLUS) {
          sim_hz += 6; if (sim_hz > MAX_SIM_HZ) sim_hz = MAX_SIM_HZ;
          sim_dt = 1.0 / sim_hz;
        }
        else if (k == SDLK_MINUS || k == SDLK_KP_MINUS) {
          sim_hz -= 6; if (sim_hz < MIN_SIM_HZ) sim_hz = MIN_SIM_HZ;
          sim_dt = 1.0 / sim_hz;
        }
        else if (k == SDLK_LEFTBRACKET)  { if (density > 0)   density -= 2; }
        else if (k == SDLK_RIGHTBRACKET) { if (density < 100) density += 2; }
        else if (k == SDLK_m) mono = !mono;
        else if (k == SDLK_F11) {
          fullscreen = !fullscreen;
          SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        }
        // Palette & cycling controls
        else if (k == SDLK_c) { g_cycle = !g_cycle; if (!g_cycle) g_hue = palette_base_hue(g_palette); }
        else if (k == SDLK_PERIOD) { if (g_cycle_speed < 360) g_cycle_speed += 10.0f; }
        else if (k == SDLK_COMMA)  { if (g_cycle_speed > 0)   g_cycle_speed -= 10.0f; }
        else if (k == SDLK_n)      set_palette(next_palette(g_palette));
        else if (k == SDLK_b)      set_palette(prev_palette(g_palette));
        else if (k == SDLK_1)      set_palette(PAL_BLUE);
        else if (k == SDLK_2)      set_palette(PAL_GREEN);
        else if (k == SDLK_3)      set_palette(PAL_PURPLE);
        else if (k == SDLK_4)      set_palette(PAL_CYAN);
        else if (k == SDLK_5)      set_palette(PAL_MAGENTA);
        else if (k == SDLK_6)      set_palette(PAL_RED);
        else if (k == SDLK_7)      set_palette(PAL_MATRIX);
        // Bloom controls
        else if (k == SDLK_v)      { BLOOM_ON = !BLOOM_ON; }
        else if (k == SDLK_g)      { BLOOM_INTENSITY += 0.1f; if (BLOOM_INTENSITY > 3.0f) BLOOM_INTENSITY = 3.0f; }
        else if (k == SDLK_f)      { BLOOM_INTENSITY -= 0.1f; if (BLOOM_INTENSITY < 0.0f) BLOOM_INTENSITY = 0.0f; }
        else if (k == SDLK_r)      { BLOOM_RADIUS += 1; if (BLOOM_RADIUS > 5) BLOOM_RADIUS = 5; }
        else if (k == SDLK_e)      { BLOOM_RADIUS -= 1; if (BLOOM_RADIUS < 1) BLOOM_RADIUS = 1; }
      }
    }

    // Resize, fullscreen, DPI change, display move.
    if (!update_layout(win, &last_dw, &last_dh)) {
      fprintf(stderr, "Font atlas rebuild failed; exiting.\n");
      goto cleanup;
    }

    // Real elapsed time; clamp so a stall (suspend, drag) doesn't fast-forward.
    double t  = now_s();
    double dt = t - prev_t;
    prev_t    = t;
    if (dt > 0.25) dt = 0.25;

    if (g_cycle) {
      g_hue += g_cycle_speed * (float)dt;
      if (g_hue >= 360.0f) g_hue = fmodf(g_hue, 360.0f);
    }

    // Fixed-timestep simulation, independent of render rate.
    if (paused) {
      acc = 0.0;
    } else {
      acc += dt;
      int steps = 0;
      while (acc >= sim_dt && steps < MAX_CATCHUP) {
        decay_cells();
        step_streams(density);
        acc -= sim_dt;
        ++steps;
      }
      if (steps == MAX_CATCHUP) acc = 0.0;  // can't keep up; drop the backlog
    }

    if (BLOOM_ON) draw_frame_with_bloom(last_dw, last_dh, mono);
    else          draw_frame_no_bloom(last_dw, last_dh, mono);
    SDL_GL_SwapWindow(win);

    // Frame limiter: with working vsync the swap already consumed the frame
    // and this does nothing; without vsync it caps at the display refresh rate.
    double remaining = display_period(win) - (now_s() - frame_start);
    if (remaining > 0.002) SDL_Delay((Uint32)((remaining - 0.001) * 1000.0));
  }

  rc = 0;

cleanup:
  free_grid();
  if (ctx) {
    if (atlas_tex) glDeleteTextures(1, &atlas_tex);
    destroy_rt(&rtA);
    destroy_rt(&rtB);
    SDL_GL_DeleteContext(ctx);
  }
  if (win) SDL_DestroyWindow(win);
  TTF_Quit();
  SDL_Quit();
  return rc;
}
