# Matrix Rain (Bloom)

A tiny SDL2 + OpenGL screensaver/toy that renders **Matrix‑style falling glyphs** with a **neon bloom** pass. CPU stays light; the GPU handles glow and blur. Works on Linux and macOS.

---

## ✨ Features

* Additive bloom via render‑to‑texture + separable blur (ping‑pong FBOs)
* Color **palettes** (`-C`) + live **hue cycling** (`--cycle`)
* Runs as a regular window, fullscreen, or **X11 desktop wallpaper** (`--desktop`)
* Glyph atlas from **SDL\_ttf** (use your own font with `-F`)
* Low CPU: grid updates on CPU, everything else on GPU

---

## 🚀 Quick Start

### Linux

```bash
sudo apt install libsdl2-dev libsdl2-ttf-dev
# Regular build
gcc -std=c11 -O2 -Wall -Wextra matrix_rain_bloom.c -o matrix_rain_bloom \
  `sdl2-config --cflags --libs` -lSDL2_ttf -lGL -lm

# With X11 desktop wallpaper support (enables --desktop)
gcc -std=c11 -O2 -Wall -Wextra -DENABLE_X11_DESKTOP matrix_rain_bloom.c -o matrix_rain_bloom \
  `sdl2-config --cflags --libs` -lSDL2_ttf -lGL -lm -lX11
```

### macOS (Homebrew)

```bash
brew install sdl2 sdl2_ttf
clang -std=c11 -O2 -Wall -Wextra \
  -I/opt/homebrew/include/SDL2 -L/opt/homebrew/lib \
  matrix_rain_bloom.c -o matrix_rain_bloom \
  -lSDL2 -lSDL2_ttf -framework OpenGL -lm
```

### Makefile (optional)

Create `Makefile` next to the source:

```make
CC      = gcc
CFLAGS  = -std=c11 -O2 -Wall -Wextra $(shell sdl2-config --cflags)
LDLIBS  = $(shell sdl2-config --libs) -lSDL2_ttf -lGL -lm
SRC     = matrix_rain_bloom.c
BIN     = matrix_rain_bloom

# Enable X11 desktop wallpaper mode with: make DESKTOP=1
ifeq ($(DESKTOP),1)
  CFLAGS += -DENABLE_X11_DESKTOP
  LDLIBS += -lX11
endif

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

.PHONY: clean run
clean:
	rm -f $(BIN)

run: $(BIN)
	./$(BIN) -f 60 -d 70 -s 18 -P 22 -C blue
```

---

## ▶️ Running

**Basic:**

```bash
./matrix_rain_bloom
```

**Blue palette, 60 FPS, medium density:**

```bash
./matrix_rain_bloom -f 60 -d 70 -s 18 -P 22 -C blue
```

**Neon look:**

```bash
./matrix_rain_bloom --bloom-scale 0.4 --bloom-radius 5 --bloom-intensity 1.8
```

**Hue sweep over time:**

```bash
./matrix_rain_bloom --cycle --cycle-speed 25
```

**Wallpaper‑style window on X11:**

```bash
# build with -DENABLE_X11_DESKTOP and -lX11 first
./matrix_rain_bloom --desktop -C matrix
```

---

## ⚙️ CLI Options

| Option                      | Meaning                                                                                                       |
| --------------------------- | ------------------------------------------------------------------------------------------------------------- |
| `-f <fps>`                  | Target FPS (default 60).                                                                                      |
| `-d <0..100>`               | Stream density percentage (default 65).                                                                       |
| `-s <cell_px>`              | Cell size in pixels (default 18).                                                                             |
| `-m`                        | Monochrome mode.                                                                                              |
| `-F <fontpath>`             | Path to TTF/OTF font for glyph atlas.                                                                         |
| `-P <pt>`                   | Font point size for atlas (default 22).                                                                       |
| `-C <palette>`              | Color palette: `blue` (default), `green`, `purple`, `cyan`, `magenta`, `red`, `matrix`.                       |
| `--cycle`                   | Enable hue cycling.                                                                                           |
| `--cycle-speed <deg/s>`     | Hue change speed (default 30).                                                                                |
| `--no-bloom`                | Disable bloom (if you just want crisp glyphs).                                                                |
| `--bloom-scale <0.25..1.0>` | Downscale factor for bloom buffer (default 0.5). Lower → softer/wider glow.                                   |
| `--bloom-radius <1..5>`     | Blur radius (default 4).                                                                                      |
| `--bloom-intensity <0..3>`  | Bloom strength (default 1.0).                                                                                 |
| `--desktop`                 | X11: attempt “desktop” window (below, sticky, skip taskbar/pager). Requires `-DENABLE_X11_DESKTOP` & `-lX11`. |

---

## ⌨️ Keyboard Controls

```
q             quit
p             pause
+ / -         FPS up / down
[ / ]         density down / up
m             toggle monochrome
F11           toggle fullscreen

c             toggle hue cycling
, / .         cycle speed down / up
n / b         next / previous palette
1..7          quick palettes (1:blue 2:green 3:purple 4:cyan 5:magenta 6:red 7:matrix)

v             toggle bloom on/off
g / f         bloom intensity up / down
r / e         blur radius up / down
```

---

## 🎨 Palettes

* `blue` (default)
* `green`
* `purple`
* `cyan`
* `magenta`
* `red`
* `matrix` (classic green)

Tip: enable `--cycle` to sweep the hue across the RGB spectrum in real time.

---

## 🔎 Fonts & Glyphs

* The program builds a texture atlas via **SDL\_ttf**.
* It tries common monospace fonts automatically (DejaVu Sans Mono, Liberation Mono, Menlo, SF Mono). Use `-F` to force a specific font.
* Adjust `-P` (point size) to sharpen glyphs for your screen scale.
* The character pool is defined in the source at `GLYPH_POOL`. You can edit it to add symbols or Kana.

---

## 🖥️ Desktop Wallpaper Mode (X11)

* Build with `-DENABLE_X11_DESKTOP` and link `-lX11`.
* Run with `--desktop` to hint your window manager to treat the window as a desktop layer (below, sticky, and hidden from taskbar/pager).
* Behavior depends on your WM/DE. Some may need additional configuration; others may not support true desktop windows.

---

## 🧪 Recipes

**Cinematic glow:**

```bash
./matrix_rain_bloom --bloom-scale 0.33 --bloom-radius 5 --bloom-intensity 2.2 -C cyan
```

**Subtle glow (low power):**

```bash
./matrix_rain_bloom --bloom-scale 0.7 --bloom-radius 3 --bloom-intensity 0.8
```

**4K dense wall of code:**

```bash
./matrix_rain_bloom -s 14 -d 80 -f 72 --bloom-scale 0.5 --bloom-radius 5 --bloom-intensity 1.6
```

---

## 🛠️ Troubleshooting

* **`fatal error: SDL_ttf.h: No such file or directory`**
  Install the dev package: `sudo apt install libsdl2-ttf-dev` (Linux) or `brew install sdl2_ttf` (macOS).

* **`undefined reference to fmodf` / `DSO missing from command line`**
  Link libm **at the end**: add `-lm` after your objects/libs.

* **`undefined reference to XFlush`** (or similar)
  Only if you compiled with `--desktop` bits: link `-lX11` and pass `-DENABLE_X11_DESKTOP`. Otherwise, run without `--desktop`.

* **Framebuffer Object functions implicitly declared**
  Your GL headers are older. Compile with `-DGL_GLEXT_PROTOTYPES` or ensure `<GL/glext.h>` is included (the source already does this on non‑Apple).

* **Black window / no glyphs**
  The fallback fonts weren’t found. Run with a known font: `-F /path/to/YourMono.ttf`.

---

## ⚡ Performance Tips

* Keep VSync on (`SDL_GL_SetSwapInterval(1)`) to avoid runaway FPS.
* Lower `--bloom-scale` for wider glow at similar cost; adjust `--bloom-intensity` to taste.
* `-s` controls grid density indirectly; smaller cells → more glyphs. Combine with `-f` carefully.
* On integrated GPUs, prefer `--bloom-radius 3..4` and `--bloom-scale 0.5..0.7`.

---

## 📄 License

Mico's Matrix Rain C code is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

Mico's Matrix Rain C code is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with  Mico's Matrix Rain C code. If not, see <https://www.gnu.org/licenses/>.

---

## 🙌 Credits

* SDL2 & SDL\_ttf
* OpenGL (compat profile)
* You, for making it glow ✨
