@echo off
setlocal enabledelayedexpansion
REM Build the MousePlotter Windows GUI logger. Prefers `zig` on PATH, otherwise
REM a vendored ..\zig-*\ copy. Flags must match windows_gui/Makefile.

REM Relative paths (log.rc's RCDATA entries, ..\tools, ..\build) assume this
REM directory; setlocal restores the caller's directory on exit.
cd /d "%~dp0"

set "ZIG="
where zig >nul 2>nul && set "ZIG=zig"
if not defined ZIG (
  for /d %%D in ("%~dp0..\zig-*") do set "ZIG=%%D\zig.exe"
)
if not defined ZIG (
  echo [error] Could not find zig. Install Zig from https://ziglang.org/download/
  echo         or place an extracted zig-*\ folder next to this repository.
  exit /b 1
)

set "PY="
where python >nul 2>nul && set "PY=python"
if not defined PY (
  where py >nul 2>nul && set "PY=py -3"
)
if not defined PY (
  echo [error] Could not find Python 3, which builds the HTML report template.
  echo         Install it from https://www.python.org/downloads/
  exit /b 1
)

REM Pinned so an arm64 host still builds the x86_64 binary.
set "TARGET=x86_64-windows-gnu"
set "CFLAGS=-O3 -fno-stack-protector"
set "LDFLAGS=-municode -Wl,--subsystem,windows -Wl,--strip-all"
set "LIBS=-lkernel32 -luser32 -lgdi32 -lshell32 -lcomdlg32 -lwinmm"

echo Using: !ZIG!
!PY! ..\tools\build_report_template.py
if errorlevel 1 ( echo [error] Report template build failed. & exit /b 1 )
"!ZIG!" rc /fo log.res log.rc
if errorlevel 1 ( echo [error] Resource compile failed. & exit /b 1 )
"!ZIG!" cc -target %TARGET% -c log.c -o log.o %CFLAGS%
if errorlevel 1 ( echo [error] Compile failed. & exit /b 1 )
"!ZIG!" cc -target %TARGET% log.o log.res -o MousePlotter.exe ^
  %LDFLAGS% %LIBS%
if errorlevel 1 ( echo [error] Link failed. & exit /b 1 )
del log.o log.res >nul 2>nul
echo Built %~dp0MousePlotter.exe
