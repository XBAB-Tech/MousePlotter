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

  OS/Browser combos that work well for in-browser recording:
  - Linux: Chromium-based browsers.
  - Windows: Chromium-based browsers.
  - macOS: Safari gives mostly uncoalesced data, though with significant
    jitter. Other browsers coalesce data.
- [`linux_cli/`](linux_cli) - Linux command-line logger. Records un-coalesced
  mouse reports via `evdev`/raw input and writes `log.csv`. Build with `make`
  (produces `mouseplotter-log`). For a portable binary to distribute (e.g. on
  GitHub Releases), use `make release` (needs [Zig](https://ziglang.org/download/)
  as a C toolchain): a static musl build with no host glibc dependency, so it
  runs on any x86_64 Linux distro/kernel.
- [`windows_gui/`](windows_gui) - Windows GUI logger, same recording logic as
  the CLI. Build with `build.bat` (needs [Zig](https://ziglang.org/download/)
  as a C toolchain) or `make`, producing `MousePlotter-Log.exe`.

## License

Root [LICENSE](LICENSE) (AGPL-3.0) covers `public/`. `linux_cli/` and
`windows_gui/` each carry their own `LICENSE` (GPL-3.0), which takes
precedence for those directories.

---

Made by [XBAB Tech](https://xbabtech.com).
