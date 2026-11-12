CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2 -g
CFLAGS  += -pthread -Icommon
LDFLAGS += -pthread

COMMON  = common/net.c common/game.c
SERVER  = server/main.c server/room.c
CLIENT  = client/main.c client/core.c client/ui.c
GUI     = client/gui.c client/core.c
HEADERS = $(wildcard common/*.h server/*.h client/*.h)

all: bubble_server bubble_client

# 圖形版 Client（需要 SDL2 與 SDL2_ttf：sudo apt install libsdl2-dev libsdl2-ttf-dev）
gui: bubble_gui

bubble_server: $(SERVER) $(COMMON) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SERVER) $(COMMON) $(LDFLAGS)

bubble_client: $(CLIENT) $(COMMON) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(CLIENT) $(COMMON) $(LDFLAGS)

bubble_gui: $(GUI) $(COMMON) $(HEADERS)
	$(CC) $(CFLAGS) $$(pkg-config --cflags sdl2 SDL2_ttf) -o $@ $(GUI) $(COMMON) $(LDFLAGS) $$(pkg-config --libs sdl2 SDL2_ttf) -lm

clean:
	rm -f bubble_server bubble_client bubble_gui

.PHONY: all gui clean
