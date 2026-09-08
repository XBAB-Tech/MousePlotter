ZIG ?= zig
PYTHON ?= python3
TARGET ?= x86_64-windows-gnu

# Keep in sync with build.bat.
CFLAGS = -O3 -fno-stack-protector
LDFLAGS = -municode -Wl,--subsystem,windows
LIBS = -lkernel32 -luser32 -lgdi32 -lshell32 -lcomdlg32 -lwinmm -ladvapi32 -lhid -lcfgmgr32

.PHONY: all test clean

all:
	$(PYTHON) ../tools/build_report_template.py
	$(ZIG) rc /fo log.res log.rc
	$(ZIG) cc -target $(TARGET) -c log.c -o log.o $(CFLAGS)
	$(ZIG) cc -target $(TARGET) -c etw.c -o etw.o $(CFLAGS)
	$(ZIG) cc -target $(TARGET) log.o etw.o log.res -o MousePlotter.exe $(LDFLAGS) $(LIBS)

# These are Windows executables; run this target on Windows (or use build.bat test).
test:
	mkdir -p ../build
	$(ZIG) cc -target $(TARGET) -O2 -Wall -Wextra -Wconversion -Wshadow tests/etw_test.c -o ../build/etw_test.exe $(LIBS)
	$(ZIG) cc -target $(TARGET) -O2 -Wall -Wextra -Wconversion -Wshadow tests/log_test.c etw.c -o ../build/log_test.exe $(LIBS)
	../build/etw_test.exe
	../build/log_test.exe

clean:
	rm -f log.o etw.o log.res MousePlotter.exe MousePlotter.pdb
