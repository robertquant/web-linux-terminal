CC      = gcc
CFLAGS  = -std=c11 -Wall -O2
LDFLAGS = -lm

TARGETS = fireworks-9040 wf3d matrixrain sortviz minesweeper fallsand lsystem g2048 poker

all: $(TARGETS)

fireworks-9040: fireworks-9040.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

wf3d: wireframe3d-8755.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

matrixrain: matrixrain-1923.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

sortviz: sortviz-1082.c
	$(CC) -std=c99 $(CFLAGS) -o $@ $< $(LDFLAGS)

minesweeper: minesweeper-8579.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

fallsand: fallsand-9948.c
	$(CC) $(CFLAGS) -o $@ $<

boids: boids-1507.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

lsystem: lsystem-9296.c
	$(CC) -std=c99 $(CFLAGS) -o $@ $< $(LDFLAGS)

g2048: g2048-2710.c
	$(CC) $(CFLAGS) -o $@ $< -lncursesw $(LDFLAGS)

poker: poker-1501.c
	$(CC) $(CFLAGS) -o $@ $< -lncurses

clean:
	rm -f $(TARGETS) boids

.PHONY: all clean
