@echo off
setlocal enabledelayedexpansion
REM Build the MousePlotter Windows GUI logger. Prefers `zig` on PATH, otherwise
REM a vendored ..\zig-*\ copy. Flags must match windows_gui/Makefile.

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

REM Pinned so an arm64 host still builds the x86_64 binary.
set "TARGET=x86_64-windows-gnu"
set "CFLAGS=-O3 -fno-stack-protector"
set "LDFLAGS=-municode -Wl,--subsystem,windows"
set "LIBS=-lkernel32 -luser32 -lgdi32 -lcomdlg32 -lwinmm"

echo Using: !ZIG!
"!ZIG!" rc /fo "%~dp0log.res" "%~dp0log.rc"
if errorlevel 1 ( echo [error] Resource compile failed. & exit /b 1 )
"!ZIG!" cc -target %TARGET% -c "%~dp0log.c" -o "%~dp0log.o" %CFLAGS%
if errorlevel 1 ( echo [error] Compile failed. & exit /b 1 )
"!ZIG!" cc -target %TARGET% "%~dp0log.o" "%~dp0log.res" -o "%~dp0MousePlotter-Log.exe" ^
  %LDFLAGS% %LIBS%
if errorlevel 1 ( echo [error] Link failed. & exit /b 1 )
del "%~dp0log.o" "%~dp0log.res" >nul 2>nul
echo Built %~dp0MousePlotter-Log.exe
