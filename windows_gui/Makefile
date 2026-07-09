ZIG ?= zig
TARGET ?= x86_64-windows-gnu

# Keep in sync with build.bat.
CFLAGS = -O3 -fno-stack-protector
LDFLAGS = -municode -Wl,--subsystem,windows
LIBS = -lkernel32 -luser32 -lgdi32 -lcomdlg32 -lwinmm

.PHONY: all clean

all:
	$(ZIG) rc /fo log.res log.rc
	$(ZIG) cc -target $(TARGET) -c log.c -o log.o $(CFLAGS)
	$(ZIG) cc -target $(TARGET) log.o log.res -o MousePlotter-Log.exe $(LDFLAGS) $(LIBS)

clean:
	rm -f log.o log.res MousePlotter-Log.exe MousePlotter-Log.pdb
