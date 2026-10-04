# Build the minimal browser.
#
# Dependencies (Debian/Ubuntu): libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev
#                               libcurl4-openssl-dev
# Dependencies (macOS, via brew): sdl2 sdl2_ttf sdl2_image curl

CXX      := g++
CC       := gcc
# -O3 + LTO: layout/shaping/decode are pure CPU on the target weak
# device, where every % counts. LTO also lets the linker drop the
# unused halves of SDL/curl glue across translation units.
# pkg-config --cflags keeps the build working when SDL lives in a
# nonstandard prefix (brew, /usr/local, custom root) — before this only
# the --libs half was honored and the header lookup silently assumed
# /usr/include.
CFLAGS_PKG := $(shell pkg-config --cflags sdl2 SDL2_ttf SDL2_image libcurl 2>/dev/null)
# FFmpeg powers the internal <video>/<audio> player (media/mediaplayer.cpp).
# Optional at build time: when the av* libs are absent the browser still
# builds and mpv remains the playback path (the media widgets show a
# "player unavailable" stage instead of video).
CFLAGS_FFM := $(shell pkg-config --cflags libavcodec libavformat libswscale libswresample libavutil 2>/dev/null)
CXXFLAGS := -std=c++17 -O3 -g -flto=auto -Wall -Wextra -Wno-unused-parameter $(CFLAGS_PKG) $(CFLAGS_FFM) -MMD -MP
CFLAGS   := -std=c99 -O3 -g -flto=auto $(CFLAGS_PKG) -MMD -MP
LDFLAGS  := $(shell pkg-config --libs sdl2 SDL2_ttf SDL2_image libcurl 2>/dev/null || echo "-lSDL2 -lSDL2_ttf -lSDL2_image -lcurl") \
            $(shell pkg-config --libs libavcodec libavformat libswscale libswresample libavutil 2>/dev/null) \
            -lpthread -flto=auto

SRCS := $(shell find . -name '*.cpp' ! -path './js/duktape/*' | sort)
OBJS := $(SRCS:.cpp=.o)
DEPS := $(OBJS:.o=.d)
DUK_OBJ := js/duktape/duktape.o

BIN  := browser

.PHONY: all clean test selftest run

all: $(BIN)

$(BIN): $(OBJS) $(DUK_OBJ)
	$(CXX) $(OBJS) $(DUK_OBJ) -o $@ $(LDFLAGS) -lm

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

js/duktape/duktape.o: js/duktape/duktape.c js/duktape/duktape.h js/duktape/duk_config.h
	$(CC) $(CFLAGS) -c $< -o $@

-include $(DEPS)

selftest: $(BIN)
	./$(BIN) --selftest

run: $(BIN)
	./$(BIN) test.html

screenshot: $(BIN)
	./$(BIN) --screenshot $(OUT) $(URL)

clean:
	rm -f $(OBJS) $(DUK_OBJ) $(DEPS) $(BIN)
