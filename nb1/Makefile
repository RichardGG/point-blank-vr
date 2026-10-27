CC ?= cc
CFLAGS ?= -O2 -std=c11 -D_GNU_SOURCE -ffp-contract=off -Wall -Wextra
SRC = nb1_video.c nb1_scene.c nb1_table.c nb1_edits.c nb1_png.c nb1_json.c nb1_surround.c nb1_sprites.c nb1_bot.c
UNAME := $(shell uname -s)

# SDL2 for the desktop viewer: sdl2-config, or pkg-config, also looking where Homebrew puts them
SDL2_CONFIG := $(firstword $(shell command -v sdl2-config 2>/dev/null) $(wildcard /opt/homebrew/bin/sdl2-config /usr/local/bin/sdl2-config))
ifneq ($(SDL2_CONFIG),)
SDL_CFLAGS := $(shell $(SDL2_CONFIG) --cflags)
SDL_LIBS := $(shell $(SDL2_CONFIG) --libs)
else
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null)
endif
ifeq ($(UNAME),Darwin)
GL_LIBS = -framework OpenGL
else
GL_LIBS = -lGL
endif

nb1_tool: $(SRC) nb1_tool.c nb1.h nb1_internal.h nb1_json.h
	$(CC) $(CFLAGS) -o $@ $(SRC) nb1_tool.c -lm -lz

pbdesk: $(SRC) desktop/pbdesk.c desktop/font7x13.h nb1.h nb1_internal.h nb1_json.h
	@if [ -z "$(SDL_LIBS)" ]; then echo; echo "SDL2 was not found. Install it, then run make pbdesk again:"; \
	echo "  macOS (Homebrew):  brew install sdl2"; echo "  Debian / Ubuntu:   sudo apt install libsdl2-dev libgl-dev"; echo; exit 1; fi
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $(SRC) desktop/pbdesk.c $(SDL_LIBS) $(GL_LIBS) -lm -lz

# PC VR host (OpenXR loader is loaded at run time; needs the OpenXR headers: apt install libopenxr-dev). Windows: see vr/README.md
pbbot: $(SRC) desktop/pbbot.c nb1.h nb1_internal.h nb1_json.h
	$(CC) $(CFLAGS) -o $@ $(SRC) desktop/pbbot.c -lm -lz

pbvr: $(SRC) vr/pbvr.c desktop/glscene.h nb1.h nb1_internal.h nb1_json.h
	$(CC) $(CFLAGS) -Wno-missing-field-initializers -Wno-unused-function -Wno-unused-variable -Wno-misleading-indentation $(SDL_CFLAGS) -o $@ $(SRC) vr/pbvr.c $(SDL_LIBS) $(GL_LIBS) -lX11 -ldl -lm -lz

clean:
	rm -f nb1_tool pbdesk pbvr pbbot
.PHONY: clean
