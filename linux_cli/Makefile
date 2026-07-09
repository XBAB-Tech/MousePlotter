.PHONY: all release clean

all:
	gcc -O3 -std=c23 -Wall -Wextra -o mouseplotter-log log.c

ZIG ?= zig

release:
	$(ZIG) cc -target x86_64-linux-musl -O3 -std=c23 -static -s -Wall -Wextra -o mouseplotter-log log.c

clean:
	rm -f mouseplotter-log
