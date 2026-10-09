# Matrix Rain (Bloom)

A tiny SDL2 + OpenGL screensaver/toy that renders **Matrix‑style falling glyphs** with a **neon bloom** pass. CPU stays light; the GPU handles glow and blur. Works on Linux and macOS.

---

## ✨ Features

* Film‑style glyphs: mirrored half‑width **katakana** mixed with digits and a few symbols (or plain ASCII with `-G ascii`)
* Glyphs **mutate** in the trails; heads start **white‑hot** and cool into the palette colour
* **Parallax depth**: up to three layers (`--layers`), far ones smaller, dimmer, slower
* **Message mode** (`-M "text"`): the rain periodically freezes into your text
* OpenGL 3.3 core shader pipeline; all glyphs drawn in one batched call per pass
* Neon bloom from the stream heads: bright pass → separable Gaussian blur (ping‑pong FBOs) → additive composite
* Smooth, continuous trail fades, independent of rain speed and frame rate
* Color **palettes** (`-C`) + live **hue cycling** (`--cycle`)
* Runs as a regular window, fullscreen, or **X11 desktop wallpaper** (`--desktop`)
* Glyph atlas from **SDL\_ttf** (use your own font with `-F`)
* Low CPU: grid updates on CPU, everything else on GPU
* HiDPI aware (cell and font sizes are in logical points)

---

## 🚀 Quick Start

Requires an OpenGL 3.3 capable GPU/driver (anything from roughly 2010 on; Mesa is fine).

### Linux

```bash
sudo apt install libsdl2-dev libsdl2-ttf-dev
make              # regular build
make DESKTOP=1    # with X11 desktop wallpaper support (enables --desktop)
make check-deps   # tells you which dev package is missing
```

### macOS (Homebrew)

```bash
brew install sdl2 sdl2_ttf
make
```

The makefile uses `pkg-config` (falling back to `sdl2-config`) and links `-framework OpenGL` on macOS, `-lGL` elsewhere.

### Web (WebAssembly, runs in the browser)

Live at **https://micomrkaic.github.io/Matrix-Rain/** — rebuilt by GitHub Actions on every push to `main` (`.github/workflows/pages.yml`).

Options go in the URL query: one‑letter keys become `-X`, longer ones `--name`, empty values are flags. Examples:

```
https://micomrkaic.github.io/Matrix-Rain/?C=matrix&layers=3
https://micomrkaic.github.io/Matrix-Rain/?C=green&M=WAKE+UP+NEO&message-every=20
https://micomrkaic.github.io/Matrix-Rain/?G=ascii&layers=1&mutate=0&no-bloom
```

In the browser: `h` help, `Enter` or double‑click fullscreen (`q` and `F11` are desktop‑only). Needs WebGL2 (all current browsers).

Building it yourself needs [Emscripten](https://emscripten.org/docs/getting_started/downloads.html):

```bash
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
~/emsdk/emsdk install 3.1.52 && ~/emsdk/emsdk activate 3.1.52
source ~/emsdk/emsdk_env.sh
make web                                  # → dist/
python3 -m http.server -d dist 8000       # open http://localhost:8000
```

The web build bundles one font, `web/fonts/NotoSansMonoCJKjp-Matrix.otf` — Noto Sans Mono CJK JP subset to ASCII + half‑width katakana (13 KB, SIL OFL 1.1, see `web/fonts/OFL.txt`).

---

## ▶️ Running

**Basic:**

```bash
./matrix_rain_bloom
```

**Blue palette, 60 steps/s, medium density:**

```bash
./matrix_rain_bloom -f 60 -d 70 -s 18 -P 22 -C blue
```

**Neon look:**

```bash
./matrix_rain_bloom --bloom-scale 0.4 --bloom-radius 5 --bloom-intensity 1.2
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
| `-f <steps/s>`              | Rain speed in simulation steps per second (6..240, default 60). Rendering follows the display refresh rate.   |
| `-d <0..100>`               | Stream density percentage (default 65).                                                                       |
| `-s <cell_pt>`              | Cell size in logical points (default 18); scaled automatically on HiDPI displays.                             |
| `-m`                        | Monochrome mode.                                                                                              |
| `-G mix\|kana\|ascii`        | Glyph set. `mix` (default): katakana + digits + a few symbols; `kana`: katakana + digits; `ascii`: Latin/symbols. |
| `--kana-font <path>`        | Font with half‑width katakana (default: found automatically, see Fonts).                                      |
| `--no-mirror`               | Don't mirror katakana (the film shows them mirrored).                                                         |
| `--mutate <0..20>`          | Glyph swaps per lit trail cell per second (default 1.5; 0 = off).                                             |
| `--layers <1..3>`           | Parallax depth layers (default 2).                                                                            |
| `-M <text>`                 | Message the rain freezes into (ASCII; word‑wrapped and centred).                                              |
| `--message-every <s>`       | Seconds between messages (default 15; 0 = show once).                                                         |
| `-F <fontpath>`             | Path to TTF/OTF font for glyph atlas.                                                                         |
| `-P <pt>`                   | Font point size for atlas (default 22); scaled automatically on HiDPI displays.                               |
| `-C <palette>`              | Color palette: `blue` (default), `green`, `purple`, `cyan`, `magenta`, `red`, `matrix`.                       |
| `--cycle`                   | Enable hue cycling.                                                                                           |
| `--cycle-speed <deg/s>`     | Hue change speed (default 30).                                                                                |
| `--no-bloom`                | Disable bloom (if you just want crisp glyphs).                                                                |
| `--bloom-scale <0.25..1.0>` | Downscale factor for bloom buffer (default 0.5). Lower → softer/wider glow.                                   |
| `--bloom-radius <1..5>`     | Blur radius (default 4).                                                                                      |
| `--bloom-intensity <0..3>`  | Bloom strength (default 1.0).                                                                                 |
| `--bloom-threshold <0..0.95>` | Trail brightness where glow starts (default 0.6). Heads always glow; lower → glow extends further down the trail. |
| `--desktop`                 | X11: attempt “desktop” window (below, sticky, skip taskbar/pager). Requires `-DENABLE_X11_DESKTOP` & `-lX11`. |

---

## ⌨️ Keyboard Controls

```
h / F1 / ?    toggle the help overlay (shows every key and its current value)
Esc           close the help overlay
q             quit
p             pause
+ / -         rain speed up / down
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
y / t         bloom threshold up / down

l             cycle depth layers 1 → 2 → 3
k             cycle glyph set (mix → kana → ascii)
x             show the message now (with -M)
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
* Katakana (U+FF66–FF9D) come from the Latin font if it has them, otherwise from a CJK font: Noto Sans CJK, IPA Gothic, Takao, Droid Sans Fallback (Linux); Hiragino or Arial Unicode (macOS); then `fc-match` as a last resort. They are rescaled to match the Latin glyph height. On Debian/Ubuntu/Pop!_OS: `sudo apt install fonts-noto-cjk`.
* If no katakana font is found, the program falls back to ASCII and says so.
* The pools are defined in the source (`ASCII_POOL`, `MIX_EXTRA`, `KANA_EXTRA`).

---

## 🖥️ Desktop Wallpaper Mode (X11)

* Build with `-DENABLE_X11_DESKTOP` and link `-lX11`.
* Run with `--desktop` to hint your window manager to treat the window as a desktop layer (below, sticky, and hidden from taskbar/pager).
* Behavior depends on your WM/DE. Some may need additional configuration; others may not support true desktop windows.

---

## 🧪 Recipes

**The film look:**

```bash
./matrix_rain_bloom -C matrix --layers 3
```

**With a message every 20 seconds:**

```bash
./matrix_rain_bloom -C green -M "WAKE UP NEO" --message-every 20
```

**Classic flat ASCII rain:**

```bash
./matrix_rain_bloom -G ascii --layers 1 --mutate 0
```

**Cinematic glow:**

```bash
./matrix_rain_bloom --bloom-scale 0.33 --bloom-radius 5 --bloom-intensity 1.4 -C cyan
```

**Subtle glow (low power):**

```bash
./matrix_rain_bloom --bloom-scale 0.7 --bloom-radius 3 --bloom-intensity 0.6
```

**4K dense wall of code:**

```bash
./matrix_rain_bloom -s 14 -d 80 -f 72 --bloom-scale 0.5 --bloom-radius 5 --bloom-intensity 1.1
```

---

## 🛠️ Troubleshooting

* **`fatal error: SDL_ttf.h: No such file or directory`**
  Install the dev package: `sudo apt install libsdl2-ttf-dev` (Linux) or `brew install sdl2_ttf` (macOS).

* **`undefined reference to fmodf` / `DSO missing from command line`**
  Link libm **at the end**: add `-lm` after your objects/libs.

* **`undefined reference to XFlush`** (or similar)
  Only if you compiled with `--desktop` bits: link `-lX11` and pass `-DENABLE_X11_DESKTOP`. Otherwise, run without `--desktop`.

* **`SDL_GL_CreateContext` fails / "needs OpenGL 3.3"**
  Your driver doesn't expose a 3.3 core context. Update Mesa / the GPU driver; on very old hardware, use the pre‑shader version of this repo.

* **GL functions implicitly declared**
  Your GL headers are older. Install a current `mesa-common-dev` / `libgl-dev` (the source defines `GL_GLEXT_PROTOTYPES` and includes `<GL/glext.h>` on non‑Apple).

* **Black window / no glyphs**
  The fallback fonts weren’t found. Run with a known font: `-F /path/to/YourMono.ttf`.

---

## ⚡ Performance Tips

* Rendering is paced by VSync; if the driver ignores VSync, a built-in limiter caps frames at the display refresh rate.
* Lower `--bloom-scale` for wider glow at similar cost; adjust `--bloom-intensity` to taste.
* `-s` controls grid density indirectly; smaller cells → more glyphs. `-f` only changes rain speed, not render cost.
* On integrated GPUs, prefer `--bloom-radius 3..4` and `--bloom-scale 0.5..0.7`.

---

## 📄 License

Mico's Matrix Rain C code is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

Mico's Matrix Rain C code is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with  Mico's Matrix Rain C code. If not, see <https://www.gnu.org/licenses/>.

---

## 🙌 Credits

* SDL2 & SDL\_ttf
* OpenGL 3.3 core
* You, for making it glow ✨
