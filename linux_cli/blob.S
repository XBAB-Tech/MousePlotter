// SPDX-FileCopyrightText: 2026 XBAB Tech, LLC
// SPDX-License-Identifier: MIT
//
// Embeds the HTML report template halves built by tools/build_report_template.py
// (paths are relative to this directory, where make runs). log.c writes
// head + CSV + tail; see save_html() there.

    .section .rodata

    .globl report_head
report_head:
    .incbin "../build/report.head.html"
    .globl report_head_end
report_head_end:

    .globl report_tail
report_tail:
    .incbin "../build/report.tail.html"
    .globl report_tail_end
report_tail_end:
