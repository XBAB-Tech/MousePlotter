# MousePlotter

Records raw mouse input and plots it, for checking polling rate, jitter, and
sensor quality.

## Layout

- [`public/`](public) - the web app, hosted at
  [mouseplotter.xbabtech.com](https://mouseplotter.xbabtech.com). Record mouse
  reports in-browser or import a CSV to plot and analyze a session. For high
  timer resolution in-browser recording, serve locally with
  `python3 serve.py` (sets the COOP/COEP headers needed for high-resolution
  timers).

  Browser/OS combos that work well for in-browser recording:
  - Chromium-based browsers on Linux
  - Chromium-based browsers on Windows
  - macOS: only Safari works, and with significant jitter and sometimes
    coalesced data
- [`linux_cli/`](linux_cli) - Linux command-line logger. Records un-coalesced
  mouse reports via `evdev`/raw input and writes `log.csv`. Build with `make`
  (produces `mouseplotter-log`).
- [`windows_gui/`](windows_gui) - Windows GUI logger, same recording logic as
  the CLI. Build with `build.bat` (needs [Zig](https://ziglang.org/download/)
  as a C toolchain) or `make`, producing `MousePlotter-Log.exe`.

## License

Root [LICENSE](LICENSE) (AGPL-3.0) covers `public/`. `linux_cli/` and
`windows_gui/` each carry their own `LICENSE` (GPL-3.0), which takes
precedence for those directories.

---

Made by [XBAB Tech](https://xbabtech.com).
