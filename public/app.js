/*
 * SPDX-FileCopyrightText: 2026 XBAB Tech, LLC
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

const $ = (id) => document.getElementById(id);
const statusIndicator = $("statusIndicator");
const statEvents = $("statEvents");
const statTotalX = $("statTotalX");
const statTotalY = $("statTotalY");
const periodSelect = $("periodSelect");
const dpiInput = $("dpiInput");
const plotDiv = $("plot");
const radioRaw = $("radioRaw");
const rawApiText = $("rawApiText");
const timerResolutionIndicator = $("timerResolutionIndicator");
const importCsvBtn = $("importCsvBtn");
const exportCsvBtn = $("exportCsvBtn");
const importCsvInput = $("importCsvInput");
const savePngBtn = $("savePngBtn");
const copyPngBtn = $("copyPngBtn");
const forceTestBtn = $("forceTestBtn");
const unsupportedNotice = $("unsupportedNotice");
const unsupportedNoticeDetail = $("unsupportedNoticeDetail");
const DEFAULT_CSV_FILENAME = "mouseplotter.csv";

const AUTO_PERIOD_LABEL = periodSelect?.options?.[0]?.textContent || "auto";
const rawSupported = "onpointerrawupdate" in window;

// Best-effort OS/browser detection. Used both to gate recording on untested
// combinations (see the "Browser/OS gating" section below) and to work
// around a Firefox-specific pointer event quirk in handlePointerEvent().
function detectOS() {
  const ua = navigator.userAgent;
  const platform = navigator.userAgentData?.platform || navigator.platform ||
    "";
  if (/win/i.test(platform) || /Windows/i.test(ua)) return "windows";
  if (
    /mac/i.test(platform) ||
    (/Mac OS X/i.test(ua) && !/iPhone|iPad|iPod/i.test(ua))
  ) {
    return "macos";
  }
  if (/linux/i.test(platform) || (/Linux/i.test(ua) && !/Android/i.test(ua))) {
    return "linux";
  }
  return "unknown";
}

function detectBrowser() {
  const ua = navigator.userAgent;
  const brands = navigator.userAgentData?.brands ?? [];
  if (/\bFirefox\/\d/.test(ua) || /\bGecko\/\d/.test(ua)) return "firefox";
  const chromiumBrand = brands.some((b) =>
    /Chromium|Google Chrome|Microsoft Edge|Opera|Brave/i.test(b.brand)
  );
  if (
    chromiumBrand ||
    /\b(Chrome|Chromium|CriOS|Edg|EdgA|OPR|SamsungBrowser)\/\d/.test(ua)
  ) {
    return "chromium";
  }
  if (
    navigator.vendor === "Apple Computer, Inc." ||
    (/\bSafari\//.test(ua) && /\bVersion\/\d/.test(ua))
  ) {
    return "safari";
  }
  return "unknown";
}

const PLOT_FONT_FAMILY = "-apple-system, system-ui, sans-serif";
const PLOT_TICK_FONT_SIZE = 14;
const PLOT_AXIS_LABEL_FONT_SIZE = 14;
const PLOT_LEGEND_FONT_SIZE = 14;
const PLOT_INFO_FONT_SIZE = 14;
const PLOT_TITLE_FONT_SIZE = 17;
const PLOT_TICK_FONT = `${PLOT_TICK_FONT_SIZE}px ${PLOT_FONT_FAMILY}`;
const PLOT_AXIS_LABEL_FONT =
  `${PLOT_AXIS_LABEL_FONT_SIZE}px ${PLOT_FONT_FAMILY}`;

const rootStyle = getComputedStyle(document.documentElement);
const THEME = {
  text: rootStyle.getPropertyValue("--text").trim() || "#0f172a",
  card: rootStyle.getPropertyValue("--card").trim() || "#ffffff",
  border: rootStyle.getPropertyValue("--border").trim() || "rgba(0,0,0,0.15)",
};

let isRecording = false;
let recordingEnabled = true; // Gated off on untested browser/OS until forced.
let data = new Float64Array(600000); // Flat array [t, x, y, ...]
let index = 0;
let currentEventType = "";
const idleText = (statusIndicator?.textContent || "").trim() ||
  "Click & hold or Space";
let isSyncingX = false;
let recordingMode = ""; // 'space' or 'mouse'
let plotTitle = "";
let countsToVelocityScale = NaN;
// Plot-only crop range (1-based, inclusive). Trims which events are plotted
// and summarized without touching the recorded data or CSV export. cropEnd
// stays Infinity ("through the last event") until data is loaded.
let cropStart = 1;
let cropEnd = Infinity;

const clampInt = (v, lo, hi) => {
  const n = Math.round(v);
  if (!Number.isFinite(n)) return lo;
  return Math.max(lo, Math.min(hi, n));
};

function resetCrop() {
  cropStart = 1;
  cropEnd = Infinity;
}

// Event count and count sums over the whole recording, ignoring the crop.
// The summary always describes the full record; only the plot honors the crop.
function fullRecordStats() {
  const count = index / 3;
  let totalX = 0;
  let totalY = 0;
  for (let i = 0; i < count; i++) {
    totalX += data[i * 3 + 1];
    totalY += data[i * 3 + 2];
  }
  return { count, totalX, totalY };
}

function ensureDataCapacity(requiredLength) {
  if (requiredLength <= data.length) return;
  let newLength = data.length;
  while (newLength < requiredLength) newLength *= 2;
  const newData = new Float64Array(newLength);
  newData.set(data);
  data = newData;
}

// Extract [t, x, y] triples for events in the 0-based inclusive range
// [loEvent, hiEvent]. Defaults cover every recorded event, so callers that
// want the full dataset (e.g. CSV export) can omit the arguments. Timestamps
// are made relative to the first event in the requested range.
function extractTriples(loEvent = 0, hiEvent = index / 3 - 1) {
  const totalCount = index / 3;
  const lo = totalCount === 0
    ? 0
    : Math.max(0, Math.min(loEvent, totalCount - 1));
  const hi = totalCount === 0
    ? -1
    : Math.max(lo, Math.min(hiEvent, totalCount - 1));
  const count = hi - lo + 1;
  if (count <= 0) {
    return {
      count: 0,
      ts: new Float64Array(0),
      mx: new Float64Array(0),
      my: new Float64Array(0),
      totalX: 0,
      totalY: 0,
      minCount: Infinity,
      maxCount: -Infinity,
    };
  }

  const t0 = data[lo * 3];
  const ts = new Float64Array(count);
  const mx = new Float64Array(count);
  const my = new Float64Array(count);
  let totalX = 0;
  let totalY = 0;
  let minCount = Infinity;
  let maxCount = -Infinity;
  for (let i = 0; i < count; i++) {
    const b = (lo + i) * 3;
    ts[i] = data[b] - t0;
    mx[i] = data[b + 1];
    my[i] = data[b + 2];
    totalX += mx[i];
    totalY += my[i];
    minCount = Math.min(minCount, mx[i], my[i]);
    maxCount = Math.max(maxCount, mx[i], my[i]);
  }
  return {
    count,
    ts,
    mx,
    my,
    totalX,
    totalY,
    minCount,
    maxCount,
  };
}

function replaceTriplesFromArrays({ ts, mx, my }) {
  if (ts.length !== mx.length || ts.length !== my.length) {
    throw new Error("Input arrays must have matching lengths.");
  }
  const count = ts.length;
  ensureDataCapacity(count * 3);
  index = 0;
  for (let i = 0; i < count; i++) {
    data[index++] = ts[i];
    data[index++] = mx[i];
    data[index++] = my[i];
  }
}

// Best-effort estimate of effective timer resolution (ms) by sampling performance.now().
function estimatePerfNowResolutionMs(samples = 4000) {
  let prev = performance.now();
  let minPositive = Infinity;

  for (let i = 0; i < samples; i++) {
    const t = performance.now();
    const d = t - prev;
    if (d > 0 && d < minPositive) minPositive = d;
    prev = t;
  }
  return Number.isFinite(minPositive) ? minPositive : NaN;
}

function updateTimerResolutionUI() {
  const resMs = estimatePerfNowResolutionMs();
  if (!Number.isNaN(resMs)) {
    const resUs = resMs * 1000;
    timerResolutionIndicator.textContent = `Timer resolution: ~${
      resUs.toFixed(resUs < 100 ? 1 : 0)
    } µs`;
  } else {
    timerResolutionIndicator.textContent = "Timer resolution: n/a";
  }
}

// Windowed kernel smoother on a resampled grid (ported from analyze.py).
// Returns rates in counts/ms for x and y.
function kernel(u) {
  const a = Math.abs(u);
  if (a >= 1.5) return 0;
  if (a > 0.5) {
    const v = 1.5 - a;
    return 0.5 * v * v;
  }
  return 0.75 - a * a;
}

function convolveValid(x, k) {
  if (k.length === 0) return new Float64Array(0);
  const outLen = x.length - k.length + 1;
  if (outLen <= 0) return new Float64Array(0);

  const out = new Float64Array(outLen);
  const kLen = k.length;
  for (let i = 0; i < outLen; i++) {
    let sum = 0;
    // Matches np.convolve(x, k, mode="valid") (i.e. k is reversed).
    for (let j = 0; j < kLen; j++) sum += x[i + j] * k[kLen - 1 - j];
    out[i] = sum;
  }
  return out;
}

function makeTimeSmoothingKernel(m = 3) {
  if (!Number.isInteger(m) || m < 1 || m % 2 === 0) {
    throw new Error("makeTimeSmoothingKernel: m must be an odd integer >= 1.");
  }

  // Mirrors:
  // k = kernel(np.linspace(-2, 2, 4*m + 1)[m//2 + 1:-(m+1)//2]); k /= k.sum()
  const step = 1 / m;
  const points = 4 * m + 1;
  const start = Math.floor(m / 2) + 1;
  const endExclusive = points - Math.floor((m + 1) / 2);
  const k = new Float64Array(Math.max(0, endExclusive - start));

  let sum = 0;
  for (let i = 0; i < k.length; i++) {
    const u = -2 + (start + i) * step;
    const w = kernel(u);
    k[i] = w;
    sum += w;
  }
  if (!(sum > 0)) return k;
  for (let i = 0; i < k.length; i++) k[i] /= sum;
  return k;
}

function smoothTimestamps(t, preferredM = 3) {
  const maxM = Math.floor(t.length / 3);
  let m = Math.min(preferredM, maxM);
  if (m % 2 === 0) m -= 1;
  if (m < 1) return { tSmoothed: t, trim: 0 };

  const k = makeTimeSmoothingKernel(m);
  const tSmoothed = convolveValid(t, k);
  const trim = Math.floor(k.length / 2);
  return { tSmoothed, trim };
}

// Estimate the motion rate (counts per ms) on a uniform output grid by placing
// a unit-area kernel at each event, scaled by that event's count, and summing:
//
//   rate(tt) = (1/h) * sum_j K((t[j] - tt) / h) * x[j],   h = windowMs / 3
//
// AREA-PRESERVATION INVARIANT: because kernel() integrates to exactly 1, the
// integral of the returned rate over time equals the sum of the input counts,
// i.e. the area under each trace is the total distance travelled -- regardless
// of how noisy or clustered the timestamps are. Each event contributes its full
// count as *area*; clustering just raises the local rate, never the total.
//
// This holds ONLY because we normalize by the fixed bandwidth h, NOT by the
// local sample density. Dividing by sum(K) instead (to make a weighted mean of
// counts) would silently destroy the invariant -- don't.
//
// The output grid is cropped to [t0 + halfW, tEnd - halfW] so that every
// emitted point has its full kernel window backed by recorded data (halfW is
// exactly the kernel support radius, 1.5*h). Points nearer than halfW to either
// end would integrate a kernel that runs off the data and bias the rate toward
// zero -- the artificial "cliff" at the trace ends. Rates are in counts/ms for
// x and y. A recording shorter than windowMs yields no fully-covered point and
// returns empty output.
function smoothRatesOnGrid(t, x, y, windowMs = 16, dtOutMs = 1 / 16) {
  if (t.length !== x.length || t.length !== y.length) {
    throw new Error(
      "smoothRatesOnGrid: input arrays must have matching lengths.",
    );
  }
  if (t.length === 0) {
    return {
      tOut: new Float64Array(0),
      xOut: new Float64Array(0),
      yOut: new Float64Array(0),
    };
  }
  if (!Number.isFinite(windowMs) || windowMs <= 0) {
    throw new Error("smoothRatesOnGrid: windowMs must be > 0.");
  }
  if (!Number.isFinite(dtOutMs) || dtOutMs <= 0) {
    throw new Error("smoothRatesOnGrid: dtOutMs must be > 0.");
  }

  const n = t.length;
  const halfW = windowMs / 2;
  const invScale = 3 / windowMs; // 1 / (windowMs / 3)
  const t0 = t[0];
  const tEnd = t[n - 1];

  // Crop to points whose full kernel window lies within [t0, tEnd] (see header
  // note). This trims halfW off each end of np.arange(t[0], t[-1], dt_out).
  const tStart = t0 + halfW;
  const tStop = tEnd - halfW;
  const m = tStop >= tStart
    ? Math.floor((tStop - tStart) / dtOutMs + 1e-12) + 1
    : 0;
  const tOut = new Float64Array(m);
  const xOut = new Float64Array(m);
  const yOut = new Float64Array(m);

  let s = 0;
  let e = 0;
  for (let i = 0; i < m; i++) {
    const tt = tStart + i * dtOutMs;
    tOut[i] = tt;

    while (s < n && t[s] <= tt - halfW) s++;
    while (e < n && t[e] <= tt + halfW) e++;

    let sx = 0;
    let sy = 0;
    for (let j = s; j < e; j++) {
      const u = Math.abs(t[j] - tt) * invScale;
      if (u >= 1.5) continue;

      const w = u > 0.5 ? 0.5 * (1.5 - u) * (1.5 - u) : 0.75 - u * u;
      sx += w * x[j];
      sy += w * y[j];
    }

    xOut[i] = sx * invScale;
    yOut[i] = sy * invScale;
  }

  return { tOut, xOut, yOut };
}

// Guess report period (ms) as multiple of 0.125ms, based on deltas where motion > 1 count
function guessPeriod(mx, my, ts) {
  const deltas = [];
  for (let i = 1; i < ts.length; i++) {
    if (Math.abs(mx[i]) > 1 || Math.abs(my[i]) > 1) {
      deltas.push(ts[i] - ts[i - 1]);
    }
  }
  if (deltas.length === 0) return 1.0;

  const bins = new Map();
  for (const d of deltas) {
    if (!Number.isFinite(d) || d <= 0) continue;
    const r = Math.round(8 * d);
    if (r <= 0) continue; // Never allow a 0ms (or negative) "period" guess.
    bins.set(r, (bins.get(r) || 0) + 1);
  }
  if (bins.size === 0) return 1.0;

  let maxCount = 0;
  let maxBin = 8;
  for (const [bin, count] of bins) {
    if (count > maxCount) {
      maxCount = count;
      maxBin = bin;
    }
  }
  return Math.max(1, maxBin) / 8;
}

// Firefox has two independent pointer-event bugs that make its normal event
// data unusable for timing:
//  - PointerEvent.timeStamp is stuck at a bogus, non-advancing value. Verified
//    with firefox-pointer-lock-test.html: performance.now() advances
//    normally in the same handler while event.timeStamp (and its delta)
//    never change, for both pointermove and pointerrawupdate.
//  - getCoalescedEvents() sub-events additionally inherit the *parent*
//    event's (broken) timeStamp per Mozilla bug 1457859, and their
//    movementX/Y don't behave like real per-sample deltas either.
// Firefox therefore skips coalescing (one sample per dispatch) and stamps
// each sample with performance.now() instead of the unusable ev.timeStamp.
// This is a coarse, frame-quantized substitute -- comparable to the existing
// Chrome/macOS display-frame limitation -- not a recovery of real per-report
// timing. Chrome and Safari are unaffected and keep using real event data.
const isFirefox = detectBrowser() === "firefox";
const useCoalescedEvents = !isFirefox;

// Stripped down handler for maximum performance
function handlePointerEvent(e) {
  const events = useCoalescedEvents && e.getCoalescedEvents
    ? e.getCoalescedEvents()
    : [e];
  ensureDataCapacity(index + events.length * 3);
  const now = isFirefox ? performance.now() : 0;
  for (const ev of events) {
    data[index++] = isFirefox ? now : ev.timeStamp;
    data[index++] = ev.movementX;
    data[index++] = ev.movementY;
  }
}

function parseCsv(text) {
  const normalized = text
    .replace(/^\uFEFF/, "")
    .replace(/\r\n/g, "\n")
    .replace(/\r/g, "\n");
  const rawLines = normalized.split("\n");
  if (rawLines.length < 3) throw new Error("CSV is too short.");

  const title = (rawLines[0] ?? "").trim();
  const dpi = parseFloat((rawLines[1] ?? "").trim());
  const dpiVal = Number.isFinite(dpi) && dpi > 0 ? dpi : NaN;

  // Line 3 (index 2) is the column header, typically "xCount,yCount,Time (ms)".
  // It's never required: data parsing simply starts at the next line, so files
  // that omit or alter the header still import.

  const ts = [];
  const mx = [];
  const my = [];
  for (let i = 3; i < rawLines.length; i++) {
    const line = rawLines[i].trim();
    if (!line) continue;
    const parts = line.split(",");
    if (parts.length < 3) continue;
    const x = parseFloat(parts[0]);
    const y = parseFloat(parts[1]);
    const t = parseFloat(parts[2]);
    if (!Number.isFinite(x) || !Number.isFinite(y) || !Number.isFinite(t)) {
      continue;
    }
    mx.push(x);
    my.push(y);
    ts.push(t);
  }

  return {
    title,
    dpi: dpiVal,
    ts: Float64Array.from(ts),
    mx: Float64Array.from(mx),
    my: Float64Array.from(my),
  };
}

function serializeCsv() {
  const { ts, mx, my, count } = extractTriples();
  const dpi = parseFloat(dpiInput.value);
  const dpiVal = Number.isFinite(dpi) && dpi > 0 ? dpi : 800;

  const formatCount = (v) => (Number.isInteger(v) ? String(v) : v.toFixed(6));
  const formatTime = (v) => (Number.isFinite(v) ? v.toFixed(6) : "0.000000");

  const titleLine = String(plotTitle || "").replace(/\r?\n/g, " ").trim() ||
    "MousePlotter";
  const lines = [
    titleLine,
    String(Math.round(dpiVal)),
    "xCount,yCount,Time (ms)",
  ];
  for (let i = 0; i < count; i++) {
    lines.push(
      `${formatCount(mx[i])},${formatCount(my[i])},${formatTime(ts[i])}`,
    );
  }
  return lines.join("\n");
}

function downloadTextFile(filename, text) {
  const blob = new Blob([text], {
    type: "text/csv;charset=utf-8",
  });
  const url = URL.createObjectURL(blob);
  try {
    const a = document.createElement("a");
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    a.remove();
  } finally {
    URL.revokeObjectURL(url);
  }
}

async function trySaveTextFileWithDialog(suggestedName, text) {
  if (!window.showSaveFilePicker) return false;
  const blob = new Blob([text], { type: "text/csv;charset=utf-8" });
  try {
    const handle = await window.showSaveFilePicker({
      suggestedName,
      types: [
        {
          description: "CSV",
          accept: { "text/csv": [".csv"] },
        },
      ],
    });
    const writable = await handle.createWritable();
    await writable.write(blob);
    await writable.close();
    return true;
  } catch (err) {
    if (err?.name === "AbortError") return true;
    throw err;
  }
}

async function importCsvFile(file) {
  if (!file) return;
  const text = await file.text();
  const parsed = parseCsv(text);
  plotTitle = parsed.title;

  if (Number.isFinite(parsed.dpi)) {
    dpiInput.value = String(Math.round(parsed.dpi));
  }
  periodSelect.value = "auto";

  replaceTriplesFromArrays({
    ts: parsed.ts,
    mx: parsed.mx,
    my: parsed.my,
  });
  resetCrop();
  renderPlot(true);
}

// ---------------------------------------------------------------------------
// Browser/OS gating
//
// High-rate pointer data is only reliable on combinations we've verified:
//  - Chromium-based browsers on Windows and Linux
//  - Safari on macOS
// Chrome on macOS coalesces motion into display frames, and Firefox is
// unreliable everywhere. On any other combination the record control is hidden
// behind a notice with a "Test anyway" escape hatch, in case an untested
// browser works (or gets fixed before this list is updated).
// ---------------------------------------------------------------------------

function isSupportedPlatform(os, browser) {
  if (os === "windows" || os === "linux") return browser === "chromium";
  if (os === "macos") return browser === "safari";
  return false;
}

function unsupportedNoticeText(os) {
  switch (os) {
    case "macos":
      return "On macOS, in-browser data recording works best on Safari. " +
        "Alternatively, use the standalone logger and import a CSV.";
    case "windows":
      return "On Windows, in-browser data recording works best on " +
        "Chromium-based browsers such as Chrome or Edge. " +
        "Alternatively, use the standalone logger and import a CSV.";
    case "linux":
      return "On Linux, in-browser data recording works best on " +
        "Chromium-based browsers such as Chrome or Chromium. " +
        "Alternatively, use the standalone logger and import a CSV.";
    default:
      return "In-browser data recording works best on Chromium-based " +
        "browsers on Windows or Linux, or Safari on macOS. Alternatively, " +
        "use the standalone logger and import a CSV.";
  }
}

function initPlatformGate() {
  const os = detectOS();
  if (isSupportedPlatform(os, detectBrowser())) return;

  recordingEnabled = false;
  if (unsupportedNoticeDetail) {
    unsupportedNoticeDetail.textContent = unsupportedNoticeText(os);
  }
  if (unsupportedNotice) unsupportedNotice.hidden = false;
}

function forceEnableRecording() {
  recordingEnabled = true;
  if (unsupportedNotice) unsupportedNotice.hidden = true;
}

// Enter pointer lock. Must be called synchronously from a user gesture.
// A no-op if already locked, so calling it again from startRecording is safe.
async function requestPointerLock() {
  if (document.pointerLockElement) return;
  try {
    await document.body.requestPointerLock({
      unadjustedMovement: true,
    });
  } catch {
    try {
      await document.body.requestPointerLock();
    } catch {
      // Ignore (may be blocked by permissions/user settings)
    }
  }
}

async function startRecording(mode = "space") {
  if (!recordingEnabled) return;
  index = 0;
  plotTitle = "";
  isRecording = true;
  recordingMode = mode;
  statusIndicator.textContent = mode === "mouse"
    ? "Recording… release to stop"
    : "Recording… Space to stop";
  statusIndicator.classList.add("recording");
  statEvents.textContent = "0";
  statTotalX.textContent = "0";
  statTotalY.textContent = "0";
  periodSelect.value = "auto";

  await requestPointerLock();

  currentEventType = radioRaw.checked && rawSupported
    ? "pointerrawupdate"
    : "pointermove";
  window.addEventListener(currentEventType, handlePointerEvent, {
    passive: true,
  });
}

function stopRecording() {
  isRecording = false;
  recordingMode = "";
  window.removeEventListener(currentEventType, handlePointerEvent);
  statusIndicator.textContent = idleText;
  statusIndicator.classList.remove("recording");
  if (document.pointerLockElement) document.exitPointerLock();
  resetCrop();
  renderPlot();
}

// ---------------------------------------------------------------------------
// Plotting (uPlot)
//
// Two stacked uPlot instances share the x (time) scale:
//  - top:    raw x/y counts as markers (left axis) + smoothed velocity lines
//            (right axis; the "vel" scale is the "y" scale times a fixed
//            counts->velocity factor, so both axes always show the same zoom)
//  - bottom: raw Δt markers + smoothed Δt line
// Raw samples and the smoothed resample grid have different time bases, so
// each chart's series are aligned onto a union x-array with uPlot.join().
// ---------------------------------------------------------------------------

const COLOR_X = "#0072B2";
const COLOR_X_SMOOTH = "#005686";
const COLOR_Y = "#D55E00";
const COLOR_Y_SMOOTH = "#A04700";
const COLOR_DT = "#009E73";
const COLOR_DT_SMOOTH = "#004F3A";
const SMOOTH_LINE_WIDTH = 2;
const PLOT_PX_ALIGN = 0;

// Fraction of the (title/legend-free) plot height used by the counts chart.
// Slightly favors the Δt panel to offset its x-axis labels and match Plotly.
const TOP_CHART_FRACTION = 0.62;
// Total width of each y axis. The bottom chart reserves the same width as
// right padding so both charts' plot areas stay horizontally aligned.
const Y_AXIS_SIZE = 44;
const Y_AXIS_LABEL_SIZE = 32;
const Y_AXIS_LABEL_GAP = 4;
const RIGHT_AXIS_WIDTH = Y_AXIS_SIZE + Y_AXIS_LABEL_SIZE;
// Tighten the bottom axis title toward the plot without clipping larger text.
const X_AXIS_SIZE = 40;
const X_AXIS_LABEL_SIZE = 20;
const X_AXIS_LABEL_GAP = -8;
const BOTTOM_CHART_TOP_PADDING = 10;

const plotHeaderEl = document.createElement("div");
plotHeaderEl.className = "plot-header";
const plotTitleEl = document.createElement("div");
plotTitleEl.className = "plot-title";
plotTitleEl.style.display = "none";
plotHeaderEl.append(plotTitleEl);
// Keep plot action buttons out of the chart area (they would cover the
// velocity axis) by hosting them in the header row next to the title.
const plotActionsEl = document.createElement("div");
plotActionsEl.className = "plot-actions";
if (savePngBtn) plotActionsEl.append(savePngBtn);
if (copyPngBtn) plotActionsEl.append(copyPngBtn);
if (plotActionsEl.childElementCount > 0) plotHeaderEl.append(plotActionsEl);
// Start/End crop controls, mirroring the PNG buttons in the opposite header
// corner. They trim the plotted/summarized event range only (see renderPlot);
// the recorded data and CSV export are unaffected.
const plotCropEl = document.createElement("div");
plotCropEl.className = "plot-crop";
plotCropEl.style.display = "none";
function makeCropField(labelText) {
  const field = document.createElement("label");
  field.className = "plot-crop__field";
  const label = document.createElement("span");
  label.className = "plot-crop__label";
  label.textContent = labelText;
  const input = document.createElement("input");
  input.type = "number";
  input.className = "plot-crop__input";
  input.min = "1";
  input.step = "1";
  input.inputMode = "numeric";
  // Scroll over the box to nudge the value by ±1 (up = increment).
  input.addEventListener("wheel", (e) => {
    if (isRecording || e.deltaY === 0) return;
    const totalCount = index / 3;
    if (totalCount === 0) return;
    e.preventDefault();
    const cur = clampInt(parseInt(input.value, 10), 1, totalCount);
    input.value = String(clampInt(cur + (e.deltaY < 0 ? 1 : -1), 1, totalCount));
    applyCropFromInputs();
  }, { passive: false });
  field.append(label, input);
  return input;
}
const cropStartInput = makeCropField("Start");
const cropEndInput = makeCropField("End");
// Reset the trim back to the full recorded range. Sits after the End field so
// the controls read Start / End / reset left-to-right as one cluster. Reuses
// resetCrop() so a click is equivalent to never having trimmed.
const cropResetBtn = document.createElement("button");
cropResetBtn.type = "button";
cropResetBtn.className = "plot-crop__reset";
cropResetBtn.title = "Reset trim to full range";
cropResetBtn.setAttribute("aria-label", "Reset trim to full range");
// Drawn as an SVG arc so it renders truly round, not the oval that font
// glyphs like "↺" produce.
cropResetBtn.innerHTML =
  '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
  'stroke-width="2" stroke-linecap="round" stroke-linejoin="round" ' +
  'aria-hidden="true"><polyline points="1 4 1 10 7 10"></polyline>' +
  '<path d="M3.51 15a9 9 0 1 0 2.13-9.36L1 10"></path></svg>';
cropResetBtn.addEventListener("click", () => {
  if (isRecording || index === 0) return;
  resetCrop();
  renderPlot(true);
});
plotCropEl.append(
  cropStartInput.parentElement,
  cropEndInput.parentElement,
  cropResetBtn,
);
plotHeaderEl.append(plotCropEl);

function updateCropUI(totalCount, lo, hi) {
  // Always visible; blank and read-only (accepts no input) until there's
  // data to crop.
  plotCropEl.style.display = "";
  const hasData = totalCount > 0;
  cropStartInput.readOnly = !hasData;
  cropEndInput.readOnly = !hasData;
  if (!hasData) {
    cropStartInput.value = "";
    cropEndInput.value = "";
    cropResetBtn.style.display = "none";
    return;
  }
  cropStartInput.max = String(totalCount);
  cropEndInput.max = String(totalCount);
  cropStartInput.value = String(lo + 1);
  cropEndInput.value = String(hi + 1);
  // Only show the reset control when the trim actually hides part of the record.
  const trimmed = lo !== 0 || hi !== totalCount - 1;
  cropResetBtn.style.display = trimmed ? "" : "none";
}

function applyCropFromInputs() {
  if (isRecording) return;
  const totalCount = index / 3;
  if (totalCount === 0) return;
  let s = clampInt(parseInt(cropStartInput.value, 10), 1, totalCount);
  let e = clampInt(parseInt(cropEndInput.value, 10), 1, totalCount);
  if (s > e) [s, e] = [e, s];
  cropStart = s;
  cropEnd = e;
  renderPlot(true);
}
cropStartInput.addEventListener("change", applyCropFromInputs);
cropEndInput.addEventListener("change", applyCropFromInputs);

const topChartEl = document.createElement("div");
topChartEl.className = "plot-chart plot-chart--top";
const sharedLegendEl = document.createElement("div");
sharedLegendEl.className = "plot-legend";
const botChartEl = document.createElement("div");
botChartEl.className = "plot-chart plot-chart--bottom";
plotDiv.append(plotHeaderEl, topChartEl, sharedLegendEl, botChartEl);
plotDiv.addEventListener("contextmenu", (e) => {
  if (!(e.target instanceof Element) || !e.target.closest("button")) {
    e.preventDefault();
  }
});

let topChart = null;
let botChart = null;
// Default ("home") ranges for every scale; restored on double-click.
let homeRanges = null;
// Δt axis ticks: multiples of the period at the home range, auto when zoomed.
let dtTicks = { linear: true, periodMs: 1 };
let plotInfoText = "";
let isSyncingTopY = false;
let syncTopYRaf = 0;

function destroyCharts() {
  cancelAnimationFrame(syncTopYRaf);
  syncTopYRaf = 0;
  topChart?.destroy();
  botChart?.destroy();
  topChart = null;
  botChart = null;
}

function hexToRgba(hex, alpha) {
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  return `rgba(${r},${g},${b},${alpha})`;
}

const roundFloat = (v) =>
  Math.abs(v) < 1e-12 ? 0 : parseFloat(v.toPrecision(12));

const fmtOrDash = (fmt) => (u, v) => (v == null ? "--" : fmt(v));
const fmtCount = fmtOrDash((v) =>
  Number.isInteger(v) ? String(v) : v.toFixed(2)
);
const fmtVel = fmtOrDash((v) => v.toFixed(3));
const fmtMs = fmtOrDash((v) => v.toFixed(3));
const fmtTick = (v) => String(roundFloat(v));

// Lossless decimation for marker-only series: keep at most one point per
// ~1px canvas cell. All markers of a series are filled as a single path, so
// duplicates at the same pixel don't change the rendered output — but they
// make full-range redraws of 100k+ events many times slower.
const DECIMATE_MIN_POINTS = 20000;

function decimatedPointsFilter(u, seriesIdx, show, gaps) {
  if (!show) return null;
  const s = u.series[seriesIdx];
  const xs = u.data[0];
  const ys = u.data[seriesIdx];
  const i0 = s.idxs?.[0] ?? 0;
  const i1 = s.idxs?.[1] ?? ys.length - 1;
  if (i1 - i0 < DECIMATE_MIN_POINTS) return null;

  const scX = u.scales.x;
  const scY = u.scales[s.scale];
  const { width, height } = u.bbox;
  const kx = width / (scX.max - scX.min);
  const ky = height / (scY.max - scY.min);
  if (!Number.isFinite(kx) || !Number.isFinite(ky)) return null;

  const margin = 8; // device px; keep points whose marker may touch the plot
  const seen = new Set();
  const out = [];
  for (let i = i0; i <= i1; i++) {
    const yv = ys[i];
    if (yv == null) continue;
    const px = Math.round((xs[i] - scX.min) * kx);
    const py = Math.round((scY.max - yv) * ky);
    if (py < -margin || py > height + margin) continue;
    const key = px * 65536 + py;
    if (!seen.has(key)) {
      seen.add(key);
      out.push(i);
    }
  }
  return out;
}

function markerSeries(label, color, scale, value) {
  return {
    label,
    scale,
    stroke: color,
    paths: () => null,
    points: {
      show: true,
      size: 4,
      width: 0,
      fill: hexToRgba(color, 0.5),
      filter: decimatedPointsFilter,
    },
    value,
    mpKind: "marker",
    mpColor: color,
  };
}

function lineSeries(label, color, scale, value) {
  return {
    label,
    scale,
    stroke: color,
    width: SMOOTH_LINE_WIDTH,
    spanGaps: true,
    points: { show: false, size: 0, width: 0 },
    value,
    mpKind: "line",
    mpColor: color,
  };
}

function getPlotLegendEntries() {
  const entries = [];
  for (const chart of [topChart, botChart]) {
    if (!chart) continue;
    chart.series.forEach((sr, seriesIdx) => {
      if (seriesIdx === 0) return;
      entries.push({
        chart,
        seriesIdx,
        label: sr.label,
        color: sr.mpColor || sr.stroke || "#000000",
        kind: sr.mpKind || "line",
        show: sr.show !== false,
      });
    });
  }
  return entries;
}

function updateLegendButtonState(btn, show) {
  btn.classList.toggle("is-off", !show);
  btn.setAttribute("aria-pressed", show ? "true" : "false");
}

function renderSharedLegend() {
  sharedLegendEl.replaceChildren();
  for (const entry of getPlotLegendEntries()) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "plot-legend__item";
    btn.title = `Toggle ${entry.label}`;
    updateLegendButtonState(btn, entry.show);

    const swatch = document.createElement("span");
    swatch.className = `plot-legend__swatch plot-legend__swatch--${entry.kind}`;
    swatch.style.setProperty("--series-color", entry.color);

    const label = document.createElement("span");
    label.className = "plot-legend__label";
    label.textContent = entry.label;

    btn.append(swatch, label);
    btn.addEventListener("click", () => {
      const show = entry.chart.series[entry.seriesIdx].show === false;
      entry.chart.setSeries(entry.seriesIdx, { show });
      updateLegendButtonState(btn, show);
    });
    sharedLegendEl.append(btn);
  }
}

// When raw and smoothed series are joined onto a union x-array, each series
// has data only at "its" x positions. Snap the cursor to the nearest non-null
// sample per series so hover values don't flicker between series.
function nearestNonNullIdx(u, seriesIdx, hoveredIdx) {
  const ys = u.data[seriesIdx];
  if (ys == null || ys.length === 0 || ys[hoveredIdx] != null) {
    return hoveredIdx;
  }
  for (let d = 1; d <= 10000; d++) {
    const lo = hoveredIdx - d;
    const hi = hoveredIdx + d;
    if (lo >= 0 && ys[lo] != null) return lo;
    if (hi < ys.length && ys[hi] != null) return hi;
    if (lo < 0 && hi >= ys.length) break;
  }
  return hoveredIdx;
}

function bindMouseDownForZoom(u, target, handler) {
  return (e) => {
    if (e.button !== 0 || e.shiftKey || e.target !== target) return;
    handler(e);
  };
}

function cursorOpts() {
  return {
    drag: { x: true, y: true },
    dataIdx: nearestNonNullIdx,
    bind: {
      mousedown: bindMouseDownForZoom,
      // Replace uPlot's zoom-out with a reset to the default ranges,
      // like Plotly's double-click "home".
      dblclick: () => () => {
        applyHomeRanges();
      },
    },
  };
}

// Mirror x-range changes (zoom/reset) onto the other chart.
function syncXScale(src) {
  if (isSyncingX) return;
  const dst = src === topChart ? botChart : topChart;
  if (!dst) return;
  const { min, max } = src.scales.x;
  if (min == null || max == null) return;
  const cur = dst.scales.x;
  if (cur.min === min && cur.max === max) return;
  isSyncingX = true;
  try {
    dst.setScale("x", { min, max });
  } finally {
    isSyncingX = false;
  }
}

function scalesMatch(a, b) {
  const span = Math.max(
    Math.abs(a.min),
    Math.abs(a.max),
    Math.abs(b.min),
    Math.abs(b.max),
    1,
  );
  const eps = span * 1e-12;
  return Math.abs(a.min - b.min) <= eps && Math.abs(a.max - b.max) <= eps;
}

function syncTopYScale(changedKey) {
  if (
    isSyncingTopY ||
    !topChart ||
    !Number.isFinite(countsToVelocityScale) ||
    countsToVelocityScale === 0
  ) return;

  const source = topChart.scales[changedKey];
  if (source?.min == null || source?.max == null) return;

  const targetKey = changedKey === "y" ? "vel" : "y";
  const next = changedKey === "y"
    ? {
      min: source.min * countsToVelocityScale,
      max: source.max * countsToVelocityScale,
    }
    : {
      min: source.min / countsToVelocityScale,
      max: source.max / countsToVelocityScale,
    };
  const target = topChart.scales[targetKey];
  if (target?.min != null && target?.max != null && scalesMatch(target, next)) {
    return;
  }

  isSyncingTopY = true;
  try {
    topChart.setScale(targetKey, next);
  } finally {
    isSyncingTopY = false;
  }
}

function queueTopYScaleSync(changedKey) {
  cancelAnimationFrame(syncTopYRaf);
  syncTopYRaf = requestAnimationFrame(() => {
    syncTopYRaf = 0;
    syncTopYScale(changedKey);
  });
}

function updateDtTickMode(u) {
  if (!homeRanges) return;
  const { min, max } = u.scales.dt;
  if (min == null || max == null) return;
  const [hMin, hMax] = homeRanges.dt;
  const eps = Math.max(1e-9, (hMax - hMin) * 1e-6);
  dtTicks.linear = Math.abs(min - hMin) < eps && Math.abs(max - hMax) < eps;
}

function applyHomeRanges() {
  if (!topChart || !botChart || !homeRanges) return;
  dtTicks.linear = true;
  isSyncingX = true;
  try {
    topChart.setScale("x", { min: homeRanges.x[0], max: homeRanges.x[1] });
    botChart.setScale("x", { min: homeRanges.x[0], max: homeRanges.x[1] });
    topChart.setScale("y", { min: homeRanges.y[0], max: homeRanges.y[1] });
    topChart.setScale("vel", {
      min: homeRanges.vel[0],
      max: homeRanges.vel[1],
    });
    botChart.setScale("dt", { min: homeRanges.dt[0], max: homeRanges.dt[1] });
  } finally {
    isSyncingX = false;
  }
}

// uPlot's global pxAlign is disabled for smoother traces, so black axis lines
// are drawn by hooks below where we can align them to the device-pixel grid.
const AXIS_BORDER = { show: false };
const AXIS_TEXT = {
  font: PLOT_TICK_FONT,
  labelFont: PLOT_AXIS_LABEL_FONT,
};
const Y_AXIS_OPTS = {
  ...AXIS_TEXT,
  size: Y_AXIS_SIZE,
  labelSize: Y_AXIS_LABEL_SIZE,
  labelGap: Y_AXIS_LABEL_GAP,
};
const X_AXIS_OPTS = {
  ...AXIS_TEXT,
  size: X_AXIS_SIZE,
  labelSize: X_AXIS_LABEL_SIZE,
  labelGap: X_AXIS_LABEL_GAP,
};

function crispStrokePos(pos, lineWidth) {
  return Math.round(pos) + (lineWidth % 2 === 1 ? 0.5 : 0);
}

function axisLineWidth() {
  return Math.max(1, Math.round(window.devicePixelRatio || 1));
}

// Solid black line at value 0, like Plotly's zeroline (counts = 0, Δt = 0).
function makeZeroLineHook(scaleKey) {
  return (u) => {
    const sc = u.scales[scaleKey];
    if (sc.min == null || sc.min > 0 || sc.max < 0) return;
    const lw = axisLineWidth();
    const y = crispStrokePos(u.valToPos(0, scaleKey, true), lw);
    const { ctx } = u;
    ctx.save();
    ctx.strokeStyle = "#000000";
    ctx.lineWidth = lw;
    ctx.beginPath();
    ctx.moveTo(u.bbox.left, y);
    ctx.lineTo(u.bbox.left + u.bbox.width, y);
    ctx.stroke();
    ctx.restore();
  };
}

function makeAxisLineHook({ left = false, right = false, bottom = false }) {
  return (u) => {
    const lw = axisLineWidth();
    const x0 = crispStrokePos(u.bbox.left, lw);
    const x1 = crispStrokePos(u.bbox.left + u.bbox.width, lw);
    const y0 = crispStrokePos(u.bbox.top, lw);
    const y1 = crispStrokePos(u.bbox.top + u.bbox.height, lw);
    const { ctx } = u;
    ctx.save();
    ctx.strokeStyle = "#000000";
    ctx.lineWidth = lw;
    ctx.beginPath();
    if (left) {
      ctx.moveTo(x0, y0);
      ctx.lineTo(x0, y1);
    }
    if (right) {
      ctx.moveTo(x1, y0);
      ctx.lineTo(x1, y1);
    }
    if (bottom) {
      ctx.moveTo(x0, y1);
      ctx.lineTo(x1, y1);
    }
    ctx.stroke();
    ctx.restore();
  };
}

// Idiomatic uPlot navigation (per the official zoom-wheel demo): the wheel
// zooms around the cursor, and middle-drag or Shift+drag pans. Plain left
// drag stays box-zoom, double-click stays reset-to-home.
const WHEEL_ZOOM_FACTOR = 0.75;

// Minimum drag length (CSS px) for an axis box-zoom to count; shorter drags
// are treated as clicks and just clear the selection.
const AXIS_ZOOM_MIN_PX = 5;

function wheelZoomFactor(e) {
  return e.deltaY < 0 ? WHEEL_ZOOM_FACTOR : 1 / WHEEL_ZOOM_FACTOR;
}

function zoomScaleAt(u, scaleKey, anchorVal, factor) {
  const sc = u.scales[scaleKey];
  if (sc?.min == null || sc?.max == null) return;
  const span = sc.max - sc.min;
  if (!(span > 0) || !Number.isFinite(anchorVal)) return;
  const pct = (anchorVal - sc.min) / span;
  const nextSpan = span * factor;
  u.setScale(scaleKey, {
    min: anchorVal - pct * nextSpan,
    max: anchorVal + (1 - pct) * nextSpan,
  });
}

function cssBbox(u) {
  const canvas = u.ctx.canvas;
  const rect = canvas.getBoundingClientRect();
  const sx = rect.width / canvas.width;
  const sy = rect.height / canvas.height;
  return {
    rect,
    left: u.bbox.left * sx,
    top: u.bbox.top * sy,
    width: u.bbox.width * sx,
    height: u.bbox.height * sy,
  };
}

function axisHit(u, { leftYScale, rightYScale = null } = {}, e) {
  const bb = cssBbox(u);
  const x = e.clientX - bb.rect.left;
  const y = e.clientY - bb.rect.top;
  const plotLeft = bb.left;
  const plotRight = bb.left + bb.width;
  const plotTop = bb.top;
  const plotBottom = bb.top + bb.height;
  const overPlot = x >= plotLeft && x <= plotRight &&
    y >= plotTop && y <= plotBottom;
  if (overPlot) return null;

  if (
    x >= plotLeft && x <= plotRight &&
    y > plotBottom && y <= bb.rect.height
  ) {
    return {
      axis: "x",
      scaleKey: "x",
      pos: x - plotLeft,
      spanPx: bb.width,
    };
  }
  if (
    leftYScale &&
    x >= 0 && x < plotLeft &&
    y >= plotTop && y <= plotBottom
  ) {
    return {
      axis: "y",
      scaleKey: leftYScale,
      pos: y - plotTop,
      spanPx: bb.height,
    };
  }
  if (
    rightYScale &&
    x > plotRight && x <= bb.rect.width &&
    y >= plotTop && y <= plotBottom
  ) {
    return {
      axis: "y",
      scaleKey: rightYScale,
      pos: y - plotTop,
      spanPx: bb.height,
    };
  }
  return null;
}

function bindWheelPanZoom(u, yScaleKeys) {
  const over = u.over;

  over.addEventListener(
    "wheel",
    (e) => {
      // Leave ctrl+wheel (browser zoom / trackpad pinch) to the browser.
      if (e.ctrlKey || e.deltaY === 0 || u.scales.x.min == null) return;
      e.preventDefault();
      const rect = over.getBoundingClientRect();
      if (rect.width <= 0 || rect.height <= 0) return;
      const factor = wheelZoomFactor(e);
      u.batch(() => {
        const xVal = u.posToVal(e.clientX - rect.left, "x");
        zoomScaleAt(u, "x", xVal, factor);
        for (const key of yScaleKeys) {
          if (u.scales[key]?.min == null) continue;
          const yVal = u.posToVal(e.clientY - rect.top, key);
          zoomScaleAt(u, key, yVal, factor);
        }
      });
    },
    { passive: false },
  );

  over.addEventListener("auxclick", (e) => {
    if (e.button === 1) e.preventDefault();
  });

  over.addEventListener("mousedown", (e) => {
    const isPanGesture = e.button === 1 || (e.button === 0 && e.shiftKey);
    if (!isPanGesture || u.scales.x.min == null) return;
    e.preventDefault();
    e.stopPropagation();
    const rect = over.getBoundingClientRect();
    if (rect.width <= 0 || rect.height <= 0) return;
    const startX = e.clientX;
    const startY = e.clientY;
    const x0 = { min: u.scales.x.min, max: u.scales.x.max };
    const xPerPx = (x0.max - x0.min) / rect.width;
    const y0 = yScaleKeys
      .filter((key) => u.scales[key].min != null)
      .map((key) => ({
        key,
        min: u.scales[key].min,
        max: u.scales[key].max,
        perPx: (u.scales[key].max - u.scales[key].min) / rect.height,
      }));
    over.classList.add("is-panning");
    const onMove = (ev) => {
      ev.preventDefault();
      const dx = (ev.clientX - startX) * xPerPx;
      u.batch(() => {
        u.setScale("x", { min: x0.min - dx, max: x0.max - dx });
        for (const s of y0) {
          const dy = (ev.clientY - startY) * s.perPx;
          u.setScale(s.key, { min: s.min + dy, max: s.max + dy });
        }
      });
    };
    const onUp = () => {
      over.classList.remove("is-panning");
      document.removeEventListener("mousemove", onMove);
      document.removeEventListener("mouseup", onUp);
    };
    document.addEventListener("mousemove", onMove, { passive: false });
    document.addEventListener("mouseup", onUp);
  });
}

function bindAxisWheelZoom(u, { leftYScale, rightYScale = null } = {}) {
  const axisOpts = { leftYScale, rightYScale };

  u.root.addEventListener(
    "wheel",
    (e) => {
      if (e.ctrlKey || e.deltaY === 0 || u.scales.x.min == null) return;

      const hit = axisHit(u, axisOpts, e);
      if (!hit) return;

      e.preventDefault();
      const factor = wheelZoomFactor(e);
      const anchorVal = u.posToVal(hit.pos, hit.scaleKey);
      zoomScaleAt(u, hit.scaleKey, anchorVal, factor);
    },
    { passive: false },
  );

  u.root.addEventListener("auxclick", (e) => {
    if (e.button === 1 && axisHit(u, axisOpts, e)) e.preventDefault();
  });

  u.root.addEventListener("mousedown", (e) => {
    if (e.button !== 1 || u.scales.x.min == null) return;
    const hit = axisHit(u, axisOpts, e);
    if (!hit) return;
    const sc = u.scales[hit.scaleKey];
    if (sc?.min == null || sc?.max == null || hit.spanPx <= 0) return;

    e.preventDefault();
    e.stopPropagation();
    const startPx = hit.axis === "x" ? e.clientX : e.clientY;
    const startRange = { min: sc.min, max: sc.max };
    const unitsPerPx = (startRange.max - startRange.min) / hit.spanPx;
    u.root.classList.add("is-panning");

    const onMove = (ev) => {
      ev.preventDefault();
      const curPx = hit.axis === "x" ? ev.clientX : ev.clientY;
      const delta = (curPx - startPx) * unitsPerPx;
      const signedDelta = hit.axis === "x" ? -delta : delta;
      u.setScale(hit.scaleKey, {
        min: startRange.min + signedDelta,
        max: startRange.max + signedDelta,
      });
    };
    const onUp = () => {
      u.root.classList.remove("is-panning");
      document.removeEventListener("mousemove", onMove);
      document.removeEventListener("mouseup", onUp);
    };
    document.addEventListener("mousemove", onMove, { passive: false });
    document.addEventListener("mouseup", onUp);
  });

  // Left-drag on an axis draws a gray selection region (like the main plot's
  // box zoom) and zooms only that axis on release, leaving the other axis
  // untouched. The zoom applies once, on mouseup — not while dragging.
  u.root.addEventListener("mousedown", (e) => {
    if (e.button !== 0 || e.shiftKey || u.scales.x.min == null) return;
    const hit = axisHit(u, axisOpts, e);
    if (!hit) return;
    const sc = u.scales[hit.scaleKey];
    if (sc?.min == null || sc?.max == null || hit.spanPx <= 0) return;

    e.preventDefault();
    e.stopPropagation();
    const overRect = u.over.getBoundingClientRect();
    if (overRect.width <= 0 || overRect.height <= 0) return;
    const isX = hit.axis === "x";
    const spanPx = isX ? overRect.width : overRect.height;
    const startPx = isX
      ? e.clientX - overRect.left
      : e.clientY - overRect.top;
    // An x-zoom affects both charts (x is shared), so mirror the band onto the
    // sibling. The charts are horizontally aligned, so the same over-relative
    // px map to the same x on both.
    const sib = isX ? (u === topChart ? botChart : topChart) : null;
    const sibH = sib ? sib.over.getBoundingClientRect().height : 0;

    // Selection edges [a, b] clamped to the plot area, in over-relative px.
    const edges = (ev) => {
      const cur = isX ? ev.clientX - overRect.left : ev.clientY - overRect.top;
      return {
        a: Math.max(0, Math.min(startPx, cur)),
        b: Math.min(spanPx, Math.max(startPx, cur)),
      };
    };
    const CLEARED = { left: 0, top: 0, width: 0, height: 0 };
    const clearSelect = () => {
      u.setSelect(CLEARED, false);
      if (sib) sib.setSelect(CLEARED, false);
    };

    const onMove = (ev) => {
      ev.preventDefault();
      const { a, b } = edges(ev);
      u.setSelect(
        isX
          ? { left: a, top: 0, width: b - a, height: overRect.height }
          : { left: 0, top: a, width: overRect.width, height: b - a },
        false,
      );
      if (sib) {
        sib.setSelect({ left: a, top: 0, width: b - a, height: sibH }, false);
      }
    };
    const onUp = (ev) => {
      document.removeEventListener("mousemove", onMove);
      document.removeEventListener("mouseup", onUp);
      const { a, b } = edges(ev);
      // Clear the gray region first so the zoom redraw doesn't flash it.
      clearSelect();
      // Ignore a plain click or a too-small drag.
      if (b - a < AXIS_ZOOM_MIN_PX) return;
      if (isX) {
        u.setScale("x", { min: u.posToVal(a, "x"), max: u.posToVal(b, "x") });
      } else {
        // Pixel space runs top-down, so the top edge is the larger value.
        u.setScale(hit.scaleKey, {
          min: u.posToVal(b, hit.scaleKey),
          max: u.posToVal(a, hit.scaleKey),
        });
      }
    };
    document.addEventListener("mousemove", onMove, { passive: false });
    document.addEventListener("mouseup", onUp);
  });

  // Double-click on an axis restores its default range only, mirroring the
  // plot-area double-click (which resets everything). The setScale hooks then
  // handle the paired concerns: x syncs to the other chart, y/vel stay locked,
  // and dt's tick mode reverts to linear.
  u.root.addEventListener("dblclick", (e) => {
    if (!homeRanges) return;
    const hit = axisHit(u, axisOpts, e);
    if (!hit) return;
    const home = homeRanges[hit.scaleKey];
    if (!home) return;
    e.preventDefault();
    e.stopPropagation();
    u.setScale(hit.scaleKey, { min: home[0], max: home[1] });
  });
}

function niceTicks(min, max, targetCount) {
  const span = max - min;
  if (!(span > 0)) return [min];
  const rawStep = span / targetCount;
  const mag = 10 ** Math.floor(Math.log10(rawStep));
  const norm = rawStep / mag;
  const step = (norm < 1.5 ? 1 : norm < 3 ? 2 : norm < 7 ? 5 : 10) * mag;
  const out = [];
  for (
    let i = Math.ceil(min / step - 1e-9);
    i * step <= max + step * 1e-9;
    i++
  ) {
    out.push(roundFloat(i * step));
  }
  return out;
}

// Δt axis: ticks at multiples of the report period while at the home range
// (like Plotly's tick0=0, dtick=period), auto "nice" ticks once zoomed.
function dtSplits(u, axisIdx, min, max) {
  const p = dtTicks.periodMs;
  if (dtTicks.linear && p > 0 && (max - min) / p <= 24) {
    const out = [];
    for (let i = Math.ceil(min / p - 1e-9); i * p <= max + p * 1e-9; i++) {
      out.push(roundFloat(i * p));
    }
    if (out.length > 1) return out;
  }
  return niceTicks(min, max, 7);
}

function chartSizes() {
  const width = Math.max(280, plotDiv.clientWidth);
  const height = Math.max(320, plotDiv.clientHeight);
  const headerH = plotHeaderEl.offsetHeight || 30;
  const legendH = sharedLegendEl.offsetHeight || 38;
  const avail = Math.max(
    340,
    height - headerH - legendH,
  );
  const topHeight = Math.round(avail * TOP_CHART_FRACTION);
  return { width, topHeight, botHeight: avail - topHeight };
}

function layoutCharts() {
  if (!topChart || !botChart) return;
  const s = chartSizes();
  topChart.setSize({ width: s.width, height: s.topHeight });
  botChart.setSize({ width: s.width, height: s.botHeight });
}

// uPlot routes scale min/max through range(). Pass explicit zoom/pan ranges
// through and only fall back to the home range when unranged (init/empty data).
const fixedRangeScale = (rangeKey, extra = {}) => ({
  ...extra,
  auto: false,
  range: (u, min, max) =>
    min == null || max == null ? homeRanges?.[rangeKey] ?? [0, 1] : [min, max],
});

function makeTopOpts(width, height) {
  return {
    width,
    height,
    pxAlign: PLOT_PX_ALIGN,
    legend: { show: false },
    scales: {
      x: fixedRangeScale("x", { time: false }),
      y: fixedRangeScale("y"),
      vel: fixedRangeScale("vel"),
    },
    series: [
      { label: "t (ms)", value: fmtMs },
      markerSeries("x counts", COLOR_X, "y", fmtCount),
      markerSeries("y counts", COLOR_Y, "y", fmtCount),
      lineSeries("x vel", COLOR_X_SMOOTH, "vel", fmtVel),
      lineSeries("y vel", COLOR_Y_SMOOTH, "vel", fmtVel),
    ],
    axes: [
      {
        scale: "x",
        size: 4,
        ticks: { show: false },
        values: (u, splits) => splits.map(() => ""),
      },
      { scale: "y", label: "counts", border: AXIS_BORDER, ...Y_AXIS_OPTS },
      {
        scale: "vel",
        label: "velocity (m/s)",
        side: 1,
        grid: { show: false },
        border: AXIS_BORDER,
        ...Y_AXIS_OPTS,
        values: (u, splits) => splits.map((v) => v.toFixed(2)),
      },
    ],
    cursor: cursorOpts(),
    hooks: {
      setScale: [
        (u, key) => {
          if (key === "x") syncXScale(u);
          else if (key === "y" || key === "vel") queueTopYScaleSync(key);
        },
      ],
      drawAxes: [
        makeZeroLineHook("y"),
        makeAxisLineHook({ left: true, right: true }),
      ],
    },
  };
}

function makeBotOpts(width, height) {
  return {
    width,
    height,
    pxAlign: PLOT_PX_ALIGN,
    legend: { show: false },
    // Right padding keeps the plot area aligned with the top chart,
    // which has the velocity axis on its right side.
    padding: [BOTTOM_CHART_TOP_PADDING, RIGHT_AXIS_WIDTH, 0, 0],
    scales: {
      x: fixedRangeScale("x", { time: false }),
      dt: fixedRangeScale("dt"),
    },
    series: [
      { label: "t (ms)", value: fmtMs },
      markerSeries("Δt raw", COLOR_DT, "dt", fmtMs),
      lineSeries("Δt smoothed", COLOR_DT_SMOOTH, "dt", fmtMs),
    ],
    axes: [
      { scale: "x", label: "time (ms)", border: AXIS_BORDER, ...X_AXIS_OPTS },
      {
        scale: "dt",
        label: "Δt (ms)",
        splits: dtSplits,
        border: AXIS_BORDER,
        ...Y_AXIS_OPTS,
        values: (u, splits) => splits.map(fmtTick),
      },
    ],
    cursor: cursorOpts(),
    hooks: {
      setScale: [
        (u, key) => {
          if (key === "x") syncXScale(u);
          else if (key === "dt") updateDtTickMode(u);
        },
      ],
      drawAxes: [
        makeZeroLineHook("dt"),
        makeAxisLineHook({ left: true }),
      ],
    },
  };
}

function mountAnnotation() {
  if (!topChart || !plotInfoText) return;
  const el = document.createElement("div");
  el.className = "plot-info-box";
  el.textContent = plotInfoText;
  topChart.over.appendChild(el);
}

function renderPlot(allowEmpty = false) {
  const totalCount = index / 3;
  if (totalCount === 0 && !allowEmpty) return;
  if (!window.uPlot) {
    plotDiv.textContent = "uPlot failed to load.";
    return;
  }

  // Resolve the crop range (1-based, inclusive) to 0-based event indices,
  // normalizing the stored values so re-renders stay stable.
  let cropLo = 0;
  let cropHi = -1;
  if (totalCount > 0) {
    cropLo = clampInt(cropStart, 1, totalCount) - 1;
    cropHi = clampInt(
      Number.isFinite(cropEnd) ? cropEnd : totalCount,
      cropLo + 1,
      totalCount,
    ) - 1;
    cropStart = cropLo + 1;
    cropEnd = cropHi + 1;
  }
  updateCropUI(totalCount, cropLo, cropHi);
  const count = totalCount === 0 ? 0 : cropHi - cropLo + 1;

  let ts;
  let mx;
  let my;
  let tSmoothed;
  let mxTrim;
  let myTrim;
  let trim = 0;
  let dtRawT;
  let dtRaw;
  let dtSmoothT;
  let dtSmooth;
  let minCount = Infinity;
  let maxCount = -Infinity;
  let guessed = 1.0;

  if (count === 0) {
    ts = new Float64Array(0);
    mx = new Float64Array(0);
    my = new Float64Array(0);
    tSmoothed = new Float64Array(0);
    mxTrim = new Float64Array(0);
    myTrim = new Float64Array(0);
    dtRawT = new Float64Array(0);
    dtRaw = new Float64Array(0);
    dtSmoothT = new Float64Array(0);
    dtSmooth = new Float64Array(0);
    minCount = -10;
    maxCount = 10;
    periodSelect.options[0].textContent = AUTO_PERIOD_LABEL;
  } else {
    // The smoothed traces (velocity + Δt) are always computed from the FULL
    // recording so that trimming via Start/End never recalculates them — a
    // crop only windows the view and trims the raw markers. Both raw and
    // smoothed share the full time base (timestamps relative to the first
    // recorded event) so they stay aligned when only part is shown.
    const full = extractTriples();
    const cropEndEx = cropHi + 1; // exclusive slice bound

    ts = full.ts.subarray(cropLo, cropEndEx);
    mx = full.mx.subarray(cropLo, cropEndEx);
    my = full.my.subarray(cropLo, cropEndEx);

    // Counts autoscale to the cropped markers only.
    for (let i = 0; i < count; i++) {
      minCount = Math.min(minCount, mx[i], my[i]);
      maxCount = Math.max(maxCount, mx[i], my[i]);
    }

    // Raw Δt markers cover the cropped range; the smoothed Δt line (below)
    // spans the whole recording.
    dtRawT = count > 1 ? new Float64Array(count - 1) : new Float64Array(0);
    dtRaw = count > 1 ? new Float64Array(count - 1) : new Float64Array(0);
    for (let i = 1; i < count; i++) {
      dtRawT[i - 1] = 0.5 * (ts[i] + ts[i - 1]);
      dtRaw[i - 1] = ts[i] - ts[i - 1];
    }

    const rec = fullRecordStats();
    statEvents.textContent = rec.count;
    statTotalX.textContent = rec.totalX;
    statTotalY.textContent = rec.totalY;

    // Auto period guess and smoothing run on the full record (crop-independent).
    ({ tSmoothed, trim } = smoothTimestamps(full.ts, 3));
    mxTrim = full.mx.subarray(trim, full.mx.length - trim);
    myTrim = full.my.subarray(trim, full.my.length - trim);

    guessed = guessPeriod(mxTrim, myTrim, tSmoothed);
    const guessedUs = guessed * 1000;
    periodSelect.options[0].textContent = guessedUs < 1000
      ? `auto (${guessedUs} µs)`
      : `auto (${guessed} ms)`;

    if (tSmoothed.length > 1) {
      dtSmoothT = new Float64Array(tSmoothed.length - 1);
      dtSmooth = new Float64Array(tSmoothed.length - 1);
      for (let i = 1; i < tSmoothed.length; i++) {
        dtSmoothT[i - 1] = 0.5 * (tSmoothed[i] + tSmoothed[i - 1]);
        dtSmooth[i - 1] = tSmoothed[i] - tSmoothed[i - 1];
      }
    } else {
      dtSmoothT = new Float64Array(0);
      dtSmooth = new Float64Array(0);
    }
  }

  const selectedPeriod = periodSelect.value === "auto"
    ? guessed
    : parseFloat(periodSelect.value);
  const periodMs = Number.isFinite(selectedPeriod) && selectedPeriod > 0
    ? selectedPeriod
    : guessed;

  const dpi = parseFloat(dpiInput.value);
  const dpiVal = Number.isFinite(dpi) && dpi > 0 ? dpi : 800;

  // Convert counts -> velocity (m/s), using:
  // (counts) * 1/dpi * (25.4/1000) * 1000/period
  const countsToVelocity = (1 / dpiVal) * (25.4 / 1000) * (1000 / periodMs);
  countsToVelocityScale = countsToVelocity;
  const countsPerMsToVelocity = (1 / dpiVal) * (25.4 / 1000) * 1000;

  const span = maxCount - minCount;
  const pad = span > 0 ? span * 0.05 : 1;
  const yMin = minCount - pad;
  const yMax = maxCount + pad;

  const SMOOTH_WINDOW_MS = 12;
  const SMOOTH_DTOUT_MS = 1 / 16;
  const SMOOTH_MAX_POINTS = 200_000;
  let dtOutMs = SMOOTH_DTOUT_MS;
  if (tSmoothed.length > 1) {
    const spanMs = tSmoothed[tSmoothed.length - 1] - tSmoothed[0];
    if (Number.isFinite(spanMs) && spanMs > 0) {
      const estimatedPoints = spanMs / dtOutMs + 2;
      if (estimatedPoints > SMOOTH_MAX_POINTS) {
        dtOutMs = spanMs / Math.max(2, SMOOTH_MAX_POINTS - 2);
      }
    }
  }

  const { tOut: tSmooth, xOut: xRate, yOut: yRate } = count > 0
    ? smoothRatesOnGrid(tSmoothed, mxTrim, myTrim, SMOOTH_WINDOW_MS, dtOutMs)
    : {
      tOut: new Float64Array(0),
      xOut: new Float64Array(0),
      yOut: new Float64Array(0),
    };
  const xVelSmooth = new Float64Array(xRate.length);
  const yVelSmooth = new Float64Array(yRate.length);
  for (let i = 0; i < xRate.length; i++) {
    xVelSmooth[i] = xRate[i] * countsPerMsToVelocity;
    yVelSmooth[i] = yRate[i] * countsPerMsToVelocity;
  }

  plotInfoText = `${Math.round(dpiVal)} dpi`;

  // Default ("home") ranges, matching the Plotly layout:
  // counts padded 5%, velocity locked to counts, Δt [0, 4 * period].
  const xHome = count > 0 ? [ts[0], ts[count - 1]] : [0, 1000];
  if (!(xHome[1] - xHome[0] > 0)) {
    xHome[0] -= 1;
    xHome[1] += 1;
  }
  homeRanges = {
    x: xHome,
    y: [yMin, yMax],
    vel: [yMin * countsToVelocity, yMax * countsToVelocity],
    dt: [0, 4 * periodMs],
  };
  dtTicks = { linear: true, periodMs };

  // Align raw samples and the smoothed grid onto shared x-arrays.
  let topData;
  let botData;
  if (count > 0) {
    topData = uPlot.join([
      [ts, mx, my],
      [tSmooth, xVelSmooth, yVelSmooth],
    ]);
  } else {
    topData = [[], [], [], [], []];
  }
  if (dtRawT.length > 0 && dtSmoothT.length > 0) {
    botData = uPlot.join([
      [dtRawT, dtRaw],
      [dtSmoothT, dtSmooth],
    ]);
  } else if (dtRawT.length > 0) {
    botData = [dtRawT, dtRaw, new Array(dtRawT.length).fill(null)];
  } else {
    botData = [[], [], []];
  }

  destroyCharts();
  plotTitleEl.textContent = plotTitle;
  plotTitleEl.style.display = plotTitle ? "" : "none";

  const sizes = chartSizes();
  topChart = new uPlot(
    makeTopOpts(sizes.width, sizes.topHeight),
    topData,
    topChartEl,
  );
  botChart = new uPlot(
    makeBotOpts(sizes.width, sizes.botHeight),
    botData,
    botChartEl,
  );
  bindWheelPanZoom(topChart, ["y", "vel"]);
  bindWheelPanZoom(botChart, ["dt"]);
  bindAxisWheelZoom(topChart, { leftYScale: "y", rightYScale: "vel" });
  bindAxisWheelZoom(botChart, { leftYScale: "dt" });
  renderSharedLegend();
  applyHomeRanges();
  // The shared legend is measurable only after creation; fix up chart heights.
  layoutCharts();
  mountAnnotation();
  // Re-measure after paint: renderPlot often runs inside an input handler
  // (e.g. the mouseup/keydown that stops recording), where container
  // measurements can be stale. The ResizeObserver won't catch this case
  // because the container's size doesn't change afterwards.
  requestAnimationFrame(() => {
    layoutCharts();
    requestAnimationFrame(layoutCharts);
  });
}

// ---------------------------------------------------------------------------
// PNG export: compose both chart canvases (plus title, legend, and info box)
// onto one canvas at the current on-screen resolution.
// ---------------------------------------------------------------------------

function drawExportLegend(ctx, width, centerY, s) {
  const entries = getPlotLegendEntries().filter((entry) => entry.show);
  if (entries.length === 0) return;

  ctx.font = `${PLOT_LEGEND_FONT_SIZE * s}px ${PLOT_FONT_FAMILY}`;
  ctx.textAlign = "left";
  ctx.textBaseline = "middle";
  const swatchW = 18 * s;
  const swatchGap = 6 * s;
  const entryGap = 22 * s;
  let total = -entryGap;
  for (const en of entries) {
    en.textW = ctx.measureText(en.label).width;
    total += swatchW + swatchGap + en.textW + entryGap;
  }
  let x = Math.max(0, (width - total) / 2);
  for (const en of entries) {
    ctx.fillStyle = en.color;
    if (en.kind === "marker") {
      ctx.beginPath();
      ctx.arc(x + swatchW / 2, centerY, 3.5 * s, 0, 2 * Math.PI);
      ctx.fill();
    } else {
      ctx.fillRect(x, centerY - 1.5 * s, swatchW, 3 * s);
    }
    ctx.fillStyle = THEME.text;
    ctx.fillText(en.label, x + swatchW + swatchGap, centerY);
    x += swatchW + swatchGap + en.textW + entryGap;
  }
}

function renderPlotPngCanvas() {
  if (!topChart || !botChart) return null;
  const cTop = topChart.ctx.canvas;
  const cBot = botChart.ctx.canvas;
  const s = window.devicePixelRatio || 1;
  const width = Math.max(cTop.width, cBot.width);
  const titleH = Math.round((plotHeaderEl.offsetHeight || 30) * s);
  const legendH = Math.round((sharedLegendEl.offsetHeight || 40) * s);
  const height = titleH + cTop.height + legendH + cBot.height;

  const canvas = document.createElement("canvas");
  canvas.width = width;
  canvas.height = height;
  const ctx = canvas.getContext("2d");
  ctx.fillStyle = THEME.card || "#ffffff";
  ctx.fillRect(0, 0, width, height);

  if (plotTitle) {
    ctx.fillStyle = THEME.text;
    ctx.font = `600 ${PLOT_TITLE_FONT_SIZE * s}px ${PLOT_FONT_FAMILY}`;
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText(plotTitle, width / 2, titleH / 2);
  }

  ctx.drawImage(cTop, 0, titleH);
  drawExportLegend(ctx, width, titleH + cTop.height + legendH / 2, s);
  ctx.drawImage(cBot, 0, titleH + cTop.height + legendH);

  // dpi/period info box, top-right of the counts plot area (device px bbox),
  // drawn as a bordered island like the live .plot-info-box overlay.
  if (plotInfoText) {
    const bb = topChart.bbox;
    const lines = plotInfoText.split("\n");
    ctx.font = `${PLOT_INFO_FONT_SIZE * s}px ${PLOT_FONT_FAMILY}`;
    const lineH = 18 * s;
    const padX = 7 * s;
    const padY = 4 * s;
    let textW = 0;
    for (const line of lines) {
      textW = Math.max(textW, ctx.measureText(line).width);
    }
    const boxW = textW + padX * 2;
    const boxH = lines.length * lineH + padY * 2;
    const boxX = bb.left + bb.width - boxW - 4 * s;
    const boxY = titleH + bb.top + 4 * s;
    ctx.fillStyle = THEME.card || "#ffffff";
    ctx.fillRect(boxX, boxY, boxW, boxH);
    ctx.strokeStyle = THEME.border || "#dcdcdc";
    ctx.lineWidth = Math.max(1, Math.round(s));
    ctx.strokeRect(boxX + 0.5, boxY + 0.5, boxW, boxH);
    ctx.fillStyle = THEME.text;
    ctx.textAlign = "right";
    ctx.textBaseline = "middle";
    lines.forEach((line, i) => {
      ctx.fillText(line, boxX + boxW - padX, boxY + padY + lineH * (i + 0.5));
    });
  }

  return canvas;
}

function plotPngBlob() {
  const canvas = renderPlotPngCanvas();
  if (!canvas) return Promise.reject(new Error("Plot is not ready"));
  return new Promise((resolve, reject) => {
    canvas.toBlob((blob) => {
      blob ? resolve(blob) : reject(new Error("Could not create PNG"));
    }, "image/png");
  });
}

// Sanitize the current plot title into a filesystem-safe filename base.
// Returns "" when there is no usable title, so callers can pick a fallback.
function sanitizedTitleBase() {
  return (plotTitle || "")
    .trim()
    .replace(/[\/\\?%*:|"<>]/g, "")
    .replace(/\s+/g, "_")
    .slice(0, 80);
}

function plotPngFilename() {
  return `${sanitizedTitleBase() || "mouseplotter"}.png`;
}

async function savePlotPng() {
  try {
    const blob = await plotPngBlob();
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = plotPngFilename();
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  } catch (err) {
    console.warn(err);
  }
}

function flashButtonLabel(btn, label) {
  if (!btn) return;
  const original = btn.dataset.originalLabel || btn.textContent;
  btn.dataset.originalLabel = original;
  btn.textContent = label;
  clearTimeout(btn._labelTimer);
  btn._labelTimer = setTimeout(() => {
    btn.textContent = btn.dataset.originalLabel;
  }, 1400);
}

async function copyPlotPng() {
  if (!navigator.clipboard?.write || typeof ClipboardItem === "undefined") {
    flashButtonLabel(copyPngBtn, "Copy failed");
    return;
  }

  try {
    const blobPromise = plotPngBlob();
    await navigator.clipboard.write([
      new ClipboardItem({ "image/png": blobPromise }),
    ]);
    flashButtonLabel(copyPngBtn, "Copied");
  } catch (err) {
    console.warn(err);
    flashButtonLabel(copyPngBtn, "Copy failed");
  }
}

window.addEventListener("keydown", (e) => {
  if (e.code !== "Space") return;
  e.preventDefault();
  e.stopPropagation();
  if (e.repeat) return;
  if (isRecording) {
    stopRecording();
  } else {
    startRecording("space");
  }
}, { capture: true });

window.addEventListener("keyup", (e) => {
  if (e.code !== "Space") return;
  e.preventDefault();
  e.stopPropagation();
}, { capture: true });

// Click-and-drag recording on the status indicator. Pointer lock engages
// immediately on press, but recording is delayed so the browser's "To show
// your cursor" dialog has finished fading in. During that hold the button
// shows a red "charging" fill (the .arming class). Releasing before the delay
// elapses cancels everything — nothing gets recorded.
const RECORD_START_DELAY_MS = 400;
let pendingRecordTimer = null;

function cancelArming() {
  pendingRecordTimer = null;
  statusIndicator.classList.remove("arming");
}

statusIndicator.addEventListener("mousedown", (e) => {
  if (e.button !== 0) return; // Only left click
  e.preventDefault();
  if (isRecording || pendingRecordTimer !== null) return;
  if (!recordingEnabled) return;
  requestPointerLock();
  statusIndicator.style.setProperty("--arm-duration", `${RECORD_START_DELAY_MS}ms`);
  statusIndicator.classList.add("arming");
  pendingRecordTimer = setTimeout(() => {
    cancelArming();
    startRecording("mouse");
  }, RECORD_START_DELAY_MS);
});

window.addEventListener("mouseup", (e) => {
  if (e.button !== 0) return;
  if (pendingRecordTimer !== null) {
    // Released within the delay: cancel, release the lock, record nothing.
    clearTimeout(pendingRecordTimer);
    cancelArming();
    if (document.pointerLockElement) document.exitPointerLock();
    return;
  }
  if (isRecording && recordingMode === "mouse") {
    stopRecording();
  }
});

if (!rawSupported) {
  radioRaw.disabled = true;
  radioRaw.checked = false;
  document.querySelector('input[value="pointermove"]').checked = true;
  rawApiText.classList.add("unsupported");
}
const rerenderIfIdle = () => !isRecording && renderPlot(true);
periodSelect.addEventListener("change", rerenderIfIdle);
dpiInput.addEventListener("change", rerenderIfIdle);

importCsvBtn.addEventListener("click", () => {
  if (isRecording) stopRecording();
  importCsvInput.value = "";
  importCsvInput.click();
});

importCsvInput.addEventListener("change", async () => {
  const file = importCsvInput.files?.[0];
  try {
    await importCsvFile(file);
  } catch (err) {
    console.error(err);
    statusIndicator.textContent = "Import failed";
    alert(err?.message || "Import failed.");
  }
});

exportCsvBtn.addEventListener("click", async () => {
  if (isRecording) stopRecording();
  const csv = serializeCsv();
  const defaultNameBase = sanitizedTitleBase();
  const suggestedName = defaultNameBase
    ? `${defaultNameBase}.csv`
    : DEFAULT_CSV_FILENAME;
  try {
    const saved = await trySaveTextFileWithDialog(suggestedName, csv);
    if (saved) return;
    const requested = prompt("Export CSV as:", suggestedName);
    if (requested == null) return;
    const filename = requested.trim();
    if (!filename) return;
    downloadTextFile(
      filename.toLowerCase().endsWith(".csv") ? filename : `${filename}.csv`,
      csv,
    );
  } catch (err) {
    console.error(err);
    alert(err?.message || "Export failed.");
  }
});

savePngBtn?.addEventListener("click", savePlotPng);
copyPngBtn?.addEventListener("click", copyPlotPng);
forceTestBtn?.addEventListener("click", forceEnableRecording);

let resizeRaf = 0;
const plotResizeObserver = new ResizeObserver(() => {
  cancelAnimationFrame(resizeRaf);
  resizeRaf = requestAnimationFrame(layoutCharts);
});
plotResizeObserver.observe(plotDiv);

initPlatformGate();
updateTimerResolutionUI();
renderPlot(true);
