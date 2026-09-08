# Windows regression tests

Run `windows_gui\build.bat test`, or `make -C windows_gui test` on Windows.
Zig is required; Python, administrator rights, and a connected mouse are not.

The tests exercise report pairing, delayed delivery beyond 64 reports, missing
events, controller/interrupter attribution, descriptor parsing, allocation
failure, partial/failed writes, and preserving a recording after a failed restart.
Shutdown uses a real consumer thread that takes over three seconds to exit.
Assertions remain enabled in optimized builds.

These are synthetic regressions, not a measurement of bus timing. Hardware
validation still needs a foreground recording while moving the selected mouse,
including load and multiple-controller cases. Check `timestampSource` and the
reported unmatched/shared counts as well as jitter; a smooth plot alone does
not establish correct attribution.
