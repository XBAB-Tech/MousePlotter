.PHONY: all release template clean

PYTHON ?= python3

all: template
	gcc -O3 -std=c23 -Wall -Wextra -o mouseplotter log.c blob.S

ZIG ?= zig

release: template
	$(ZIG) cc -target x86_64-linux-musl -O3 -std=c23 -static -s -Wall -Wextra -o mouseplotter log.c blob.S

# blob.S embeds the ../build/ output of this script.
template:
	$(PYTHON) ../tools/build_report_template.py

clean:
	rm -f mouseplotter
