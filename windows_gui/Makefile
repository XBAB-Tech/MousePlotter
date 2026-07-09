# Build MousePlotter-Log.exe with zig cc (a drop-in C compiler).
# If zig is not on PATH:  make ZIG=..\zig-x86_64-windows-0.17.0-dev.1275+59a628c6d\zig.exe
#
# -target selects the Windows/MinGW headers and libs regardless of host OS,
# so this also cross-compiles from Linux or macOS.
#
# Two steps: compile normally (Windows headers available), then link with
# -nostdlib so the exe uses no C runtime and depends only on core Windows DLLs
# (runs on Windows 7 through 11 with nothing installed).
ZIG ?= zig
TARGET ?= x86_64-windows-gnu

all:
	$(ZIG) cc -target $(TARGET) -c log.c -o log.o -O2 -fno-stack-protector
	$(ZIG) cc -target $(TARGET) log.o -o MousePlotter-Log.exe -nostdlib -Wl,--subsystem,windows -lkernel32 -luser32 -lgdi32 -lcomdlg32 -lwinmm -lpowrprof
