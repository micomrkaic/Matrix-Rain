CC      = gcc
CFLAGS  = -std=c11 -O2 -Wall -Wextra $(shell sdl2-config --cflags)
LDLIBS  = $(shell sdl2-config --libs) -lSDL2_ttf -lGL -lm

SRC     = matrix_rain_bloom.c
BIN     = matrix_rain_bloom

# Enable X11 desktop wallpaper mode with: make DESKTOP=1
ifeq ($(DESKTOP),1)
  CFLAGS  += -DENABLE_X11_DESKTOP
  LDLIBS  += -lX11
endif

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

.PHONY: clean run
clean:
	rm -f $(BIN)

run: $(BIN)
	./$(BIN) -f 60 -d 70 -s 18 -P 22 -C blue
