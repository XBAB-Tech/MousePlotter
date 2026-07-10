# MousePlotter

Records raw mouse input and plots it, for checking polling rate, jitter, and
sensor quality. Inspired by [MouseTester](https://github.com/microe1/MouseTester).

- [`public/`](public) - the web app, hosted at
  [mouseplotter.xbabtech.com](https://mouseplotter.xbabtech.com). Record mouse
  reports in-browser or import a CSV to plot and analyze a session. To serve
  locally with high timer resolution in-browser recording, run
  `python3 serve.py` (sets the COOP/COEP headers needed for high-resolution
  timers).

  OS/Browser combos that work well for in-browser recording:
  - Linux: Chromium-based browsers.
  - Windows: Chromium-based browsers.
  - macOS: Safari gives mostly uncoalesced data, though with significant
    jitter. Other browsers coalesce data.
- [`linux_cli/`](linux_cli) - Linux command-line logger. Records un-coalesced
  mouse reports via `evdev`/raw input, then saves a CSV or a self-contained
  HTML report that opens in the browser with the capture already plotted.
  Build with `make` (produces `mouseplotter`; needs Python 3, which
  builds the report template from `public/`). For a portable binary to
  distribute, use `make release` (needs
  [Zig](https://ziglang.org/download/) as a C toolchain).
- [`windows_gui/`](windows_gui) - Windows GUI logger, same recording logic
  and save options as the CLI. Build with `build.bat` (needs
  [Zig](https://ziglang.org/download/) as a C toolchain and Python 3) or
  `make`, producing `MousePlotter.exe`.

## License

Root [LICENSE](LICENSE) (AGPL-3.0) covers `public/`. `linux_cli/` and
`windows_gui/` each carry their own `LICENSE` (MIT), which takes
precedence for those directories.

---

Made by [XBAB Tech](https://xbabtech.com).
