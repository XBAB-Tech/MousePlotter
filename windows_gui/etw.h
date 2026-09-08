// SPDX-FileCopyrightText: 2026 XBAB Tech, LLC
// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

// Select the physical USB device behind a Raw Input mouse and start tracing.
// Returns -1 when identity, elevation, or session resources are unavailable.
int etw_start(HANDLE mouse);
void etw_stop(void); // drains and joins the consumer; safe when not running

enum etw_source { ETW_RAW, ETW_COMPLETION, ETW_INTERRUPT };
struct etw_time { int64_t t; enum etw_source source; };
struct etw_pairing {
    struct etw_time *times; // exactly one per Raw Input sample, in sample order
    size_t interrupts, completions, raw;
    size_t unmatched;      // extra USB completions between matched samples
    size_t shared;         // repeated interrupt times: unresolved bus timing
    double lag_us, resid_us;
};

// Match by completion order, then use the associated interrupt time if known.
// Preserves sample order/count; never adjusts timestamps to a polling cadence.
// Ambiguous samples retain their own QPC timestamp and are marked ETW_RAW.
// Returns -1 if tracing failed or no samples could be matched. Call after stop.
int etw_pair(const int64_t *user_t, size_t n, struct etw_pairing *out, int64_t qpf);
void etw_free_pairing(struct etw_pairing *p);
