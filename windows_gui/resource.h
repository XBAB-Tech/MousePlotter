// SPDX-FileCopyrightText: 2026 XBAB Tech, LLC
// SPDX-License-Identifier: MIT
//
// Resource IDs shared by log.c and log.rc.

#pragma once

// The shell shows the numerically lowest icon ID as the file's icon, so the
// application icon must stay at 1.
#define IDI_APPICON 1

// HTML report template halves built by tools/build_report_template.py; the
// report file is head + CSV + tail (see save_html in log.c).
#define IDR_REPORT_HEAD 2
#define IDR_REPORT_TAIL 3
