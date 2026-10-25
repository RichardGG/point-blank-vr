CC ?= cc
CFLAGS ?= -O2 -std=c11 -D_GNU_SOURCE -ffp-contract=off -Wall -Wextra
SRC = nb1_video.c nb1_scene.c nb1_table.c nb1_edits.c nb1_png.c nb1_json.c
nb1_tool: $(SRC) nb1_tool.c nb1.h nb1_internal.h nb1_json.h
	$(CC) $(CFLAGS) -o $@ $(SRC) nb1_tool.c -lm -lz
clean:
	rm -f nb1_tool
.PHONY: clean
