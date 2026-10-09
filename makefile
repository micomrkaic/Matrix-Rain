CC       = cc
SRC      = matrix_rain_bloom.c
BIN      = matrix_rain_bloom

UNAME_S := $(shell uname -s)

# Prefer pkg-config (works on Linux and Homebrew); fall back to sdl2-config.
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_ttf 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs   sdl2 SDL2_ttf 2>/dev/null)
ifeq ($(strip $(SDL_LIBS)),)
  SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
  SDL_LIBS   := $(shell sdl2-config --libs 2>/dev/null) -lSDL2_ttf
endif

ifeq ($(UNAME_S),Darwin)
  GL_LIBS = -framework OpenGL
else
  GL_LIBS = -lGL
endif

CFLAGS  += -std=c11 -O2 -Wall -Wextra $(SDL_CFLAGS)
LDLIBS  += $(SDL_LIBS) $(GL_LIBS) -lm

# Enable X11 desktop wallpaper mode with: make DESKTOP=1
ifeq ($(DESKTOP),1)
  CFLAGS += -DENABLE_X11_DESKTOP
  LDLIBS += -lX11
endif

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

# ---- Web build (Emscripten → WebGL2) ----
# Needs emcc on PATH (source emsdk_env.sh). Output goes to dist/, ready for
# GitHub Pages or any static web server: make web && (cd dist && python3 -m http.server)
EMCC     ?= emcc
WEB_DIR   = web
WEB_OUT   = dist
WEB_FLAGS = -std=c11 -O2 -Wall -Wextra \
            -sUSE_SDL=2 -sUSE_SDL_TTF=2 \
            -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 \
            -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web \
            --shell-file $(WEB_DIR)/shell.html \
            --preload-file $(WEB_DIR)/fonts@/fonts

web: $(WEB_OUT)/index.html

$(WEB_OUT)/index.html: $(SRC) $(WEB_DIR)/shell.html $(wildcard $(WEB_DIR)/fonts/*)
	mkdir -p $(WEB_OUT)
	$(EMCC) $(WEB_FLAGS) $(SRC) -o $@
	cp $(WEB_DIR)/fonts/OFL.txt $(WEB_OUT)/FONT-LICENSE.txt

.PHONY: clean run check-deps web
clean:
	rm -f $(BIN)
	rm -rf $(WEB_OUT)

run: $(BIN)
	./$(BIN) -f 60 -d 70 -s 18 -P 22 -C blue

# Quick sanity check for build dependencies
check-deps:
	@pkg-config --exists sdl2     || echo "missing: SDL2 dev (apt: libsdl2-dev | brew: sdl2)"
	@pkg-config --exists SDL2_ttf || echo "missing: SDL2_ttf dev (apt: libsdl2-ttf-dev | brew: sdl2_ttf)"
	@echo "check-deps done"
