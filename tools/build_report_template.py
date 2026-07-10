#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 XBAB Tech, LLC
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Build the self-contained HTML report template from public/.
#
# Emits build/report.head.html and build/report.tail.html. A report is
# head + raw CSV + tail: the standalone loggers write the head blob, stream
# their CSV rows byte-for-byte into the embedded data block, then write the
# tail blob. Neither logger contains any HTML; if a marker or tag this script
# relies on disappears from public/, the build fails here, not at runtime.

import base64
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PUBLIC = ROOT / "public"
BUILD = ROOT / "build"

CSV_OPEN_TAG = '<script type="application/csv" id="embeddedCsv">'


def die(msg):
    sys.exit(f"build_report_template: error: {msg}")


def replace_once(html, needle, replacement):
    n = html.count(needle)
    if n != 1:
        die(f"expected exactly 1 occurrence of {needle!r} in index.html, found {n}")
    return html.replace(needle, replacement)


def read_text(name):
    return (PUBLIC / name).read_text(encoding="utf-8")


def inline_js(html, name):
    js = read_text(name)
    # Raw text inside <script> ends at the first "</script"; "<!--" starts an
    # escaped region with its own parsing rules. Neither may appear in the body.
    for bad in ("</script", "<!--"):
        if bad in js:
            die(f"{name} contains {bad!r} and cannot be inlined")
    return replace_once(
        html, f'<script src="{name}"></script>', f"<script>\n{js}\n</script>"
    )


def inline_css(html, name):
    css = read_text(name)
    if "</style" in css:
        die(f"{name} contains '</style' and cannot be inlined")
    return replace_once(
        html,
        f'<link rel="stylesheet" href="{name}" />',
        f"<style>\n{css}\n</style>",
    )


def inline_svg(html, ref):
    data = (PUBLIC / ref).read_bytes()
    uri = "data:image/svg+xml;base64," + base64.b64encode(data).decode("ascii")
    return replace_once(html, f'"{ref}"', f'"{uri}"')


def main():
    html = (PUBLIC / "index.html").read_text(encoding="utf-8")

    html = inline_css(html, "uPlot.min.css")
    html = inline_css(html, "styles.css")
    html = inline_js(html, "uPlot.iife.min.js")

    for icon in ("favicon.svg", "logo.svg", "discord.svg", "github.svg"):
        html = inline_svg(html, f"assets/icons/{icon}")

    # Drop the whole "About MousePlotter" section: it's teaching material and
    # in-browser-recording instructions for the live site, and carries the
    # only <img> references to assets/images/.
    start_marker = "<!-- report:strip -->"
    end_marker = "<!-- /report:strip -->"
    for marker in (start_marker, end_marker):
        if html.count(marker) != 1:
            die(f"expected exactly 1 occurrence of {marker!r} in index.html")
    start = html.index(start_marker)
    end = html.index(end_marker) + len(end_marker)
    if end <= start:
        die("report:strip markers are out of order")
    html = html[:start] + html[end:]

    # The data block goes where the marker sits, just before the inlined
    # app.js, so app.js can read it synchronously at top level.
    html = replace_once(html, "<!-- report:csv -->", CSV_OPEN_TAG)
    html = inline_js(html, "app.js")

    leftover = [
        marker
        for marker in ("assets/", '<link rel="stylesheet"', "report:", "<script src")
        if marker in html
    ]
    if leftover:
        die(f"unexpected external references remain in the template: {leftover}")

    head, _, rest = html.partition(CSV_OPEN_TAG)
    # The head blob must end exactly at the '>' of the opening tag: any
    # trailing byte becomes the first line of the CSV and breaks the title.
    head += CSV_OPEN_TAG
    tail = "</script>" + rest

    BUILD.mkdir(exist_ok=True)
    (BUILD / "report.head.html").write_text(head, encoding="utf-8", newline="")
    (BUILD / "report.tail.html").write_text(tail, encoding="utf-8", newline="")
    print(
        f"build_report_template: wrote {BUILD / 'report.head.html'} "
        f"({len(head.encode())} bytes) and {BUILD / 'report.tail.html'} "
        f"({len(tail.encode())} bytes)"
    )


if __name__ == "__main__":
    main()
