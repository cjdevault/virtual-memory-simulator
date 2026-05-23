# CSc 452 Project 5 — Virtual Memory Simulator
CC      = gcc
CFLAGS  = -std=c11 -O2 -Wall -Wextra -pedantic
LDFLAGS =

vmsim: vmsim.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o vmsim vmsim.c

clean:
	rm -f vmsim

.PHONY: clean
