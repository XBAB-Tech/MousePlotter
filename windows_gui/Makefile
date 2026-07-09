ZIG ?= zig
TARGET ?= x86_64-windows-gnu

.PHONY: all clean

all:
	$(ZIG) cc -target $(TARGET) -c log.c -o log.o -O2 -fno-stack-protector
	$(ZIG) cc -target $(TARGET) log.o -o MousePlotter-Log.exe -nostdlib -Wl,--subsystem,windows -Wl,--strip-all -lkernel32 -luser32 -lgdi32 -lcomdlg32 -lwinmm -lpowrprof

clean:
	rm -f log.o MousePlotter-Log.exe
