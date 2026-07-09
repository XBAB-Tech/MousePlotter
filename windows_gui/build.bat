@echo off
setlocal enabledelayedexpansion
REM Build the MousePlotter Windows GUI logger with zig cc.
REM Prefers `zig` on PATH; otherwise falls back to a vendored ..\zig-*\ copy.

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

echo Using: !ZIG!
REM Two steps: compile normally (Windows headers available), then link with
REM -nostdlib so the exe uses no C runtime and depends only on core Windows DLLs
REM (runs on Windows 7 through 11 with nothing installed).
"!ZIG!" cc -c "%~dp0log.c" -o "%~dp0log.o" -O2 -fno-stack-protector
if errorlevel 1 ( echo [error] Compile failed. & exit /b 1 )
"!ZIG!" cc "%~dp0log.o" -o "%~dp0MousePlotter-Log.exe" -nostdlib ^
  -Wl,--subsystem,windows ^
  -lkernel32 -luser32 -lgdi32 -lcomdlg32 -lwinmm -lpowrprof
if errorlevel 1 ( echo [error] Link failed. & exit /b 1 )
del "%~dp0log.o" >nul 2>nul
echo Built %~dp0MousePlotter-Log.exe
