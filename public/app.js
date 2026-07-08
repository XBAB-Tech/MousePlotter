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
const DEFAULT_CSV_FILENAME = "mouseplotter.csv";

const AUTO_PERIOD_LABEL = periodSelect?.options?.[0]?.textContent || "auto";
const rawSupported = "onpointerrawupdate" in window;
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
let data = new Float64Array(600000); // Flat array [t, x, y, ...]
let index = 0;
let currentEventType = "";
const idleText = (statusIndicator?.textContent || "").trim() ||
  "Click & hold or Space";
let isSyncingX = false;
let recordingMode = ""; // 'space' or 'mouse'
let plotTitle = "";
let countsToVelocityScale = NaN;

function ensureDataCapacity(requiredLength) {
  if (requiredLength <= data.length) return;
  let newLength = data.length;
  while (newLength < requiredLength) newLength *= 2;
  const newData = new Float64Array(newLength);
  newData.set(data);
  data = newData;
}

function extractTriples() {
  const count = index / 3;
  if (count === 0) {
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

  const t0 = data[0];
  const ts = new Float64Array(count);
  const mx = new Float64Array(count);
  const my = new Float64Array(count);
  let totalX = 0;
  let totalY = 0;
  let minCount = Infinity;
  let maxCount = -Infinity;
  for (let i = 0; i < count; i++) {
    ts[i] = data[i * 3] - t0;
    mx[i] = data[i * 3 + 1];
    my[i] = data[i * 3 + 2];
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
    }us`;
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

  // Mirrors: np.arange(t[0], t[-1] + dt_out, dt_out)
  const m = Math.max(0, Math.floor((tEnd - t0) / dtOutMs + 1e-12) + 2);
  const tOut = new Float64Array(m);
  const xOut = new Float64Array(m);
  const yOut = new Float64Array(m);

  let s = 0;
  let e = 0;
  for (let i = 0; i < m; i++) {
    const tt = t0 + i * dtOutMs;
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

// Stripped down handler for maximum performance
function handlePointerEvent(e) {
  const events = e.getCoalescedEvents ? e.getCoalescedEvents() : [e];
  ensureDataCapacity(index + events.length * 3);
  for (const ev of events) {
    data[index++] = ev.timeStamp;
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

  // Header line (typically: xCount,yCount,Time (ms))
  const header = (rawLines[2] ?? "").trim();
  const looksLikeHeader = /xcount/i.test(header) && /ycount/i.test(header) &&
    /time/i.test(header);
  if (!looksLikeHeader) {
    // Still allow import; some files may omit/alter the header.
  }

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
  statusIndicator.textContent = `Imported ${parsed.ts.length} samples`;
  renderPlot(true);
}

async function startRecording(mode = "space") {
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

  if (!document.pointerLockElement) {
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
        makeAxisLineHook({ left: true, bottom: true }),
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
  const count = index / 3;
  if (count === 0 && !allowEmpty) return;
  if (!window.uPlot) {
    plotDiv.textContent = "uPlot failed to load.";
    return;
  }

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
  let totalX = 0;
  let totalY = 0;
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
    const extracted = extractTriples();
    ({ ts, mx, my } = extracted);
    dtRawT = count > 1 ? new Float64Array(count - 1) : new Float64Array(0);
    dtRaw = count > 1 ? new Float64Array(count - 1) : new Float64Array(0);

    totalX = extracted.totalX;
    totalY = extracted.totalY;
    minCount = extracted.minCount;
    maxCount = extracted.maxCount;
    for (let i = 1; i < count; i++) {
      dtRawT[i - 1] = 0.5 * (ts[i] + ts[i - 1]);
      dtRaw[i - 1] = ts[i] - ts[i - 1];
    }

    statEvents.textContent = count;
    statTotalX.textContent = totalX;
    statTotalY.textContent = totalY;

    // Update period dropdown: set auto option text to guessed value
    ({ tSmoothed, trim } = smoothTimestamps(ts, 3));
    mxTrim = mx.subarray(trim, mx.length - trim);
    myTrim = my.subarray(trim, my.length - trim);

    guessed = guessPeriod(mxTrim, myTrim, tSmoothed);
    const guessedUs = guessed * 1000;
    periodSelect.options[0].textContent = guessedUs < 1000
      ? `auto (${guessedUs}us)`
      : `auto (${guessed}ms)`;

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
  if (count > 1) {
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

  let periodLabel = "auto";
  if (Number.isFinite(periodMs) && periodMs > 0) {
    const digits = periodMs < 1 ? 3 : periodMs < 10 ? 2 : 1;
    periodLabel = `${periodMs.toFixed(digits).replace(/\.?0+$/, "")}ms`;
  }
  const dpiText = `${Math.round(dpiVal)}dpi`;
  plotInfoText = `${dpiText}\n${periodLabel}`;

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

function plotPngFilename() {
  const nameBase = (plotTitle || "")
    .trim()
    .replace(/[\/\\?%*:|"<>]/g, "")
    .replace(/\s+/g, "_")
    .slice(0, 80);
  return `${nameBase || "mouseplotter"}.png`;
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

// Click-and-drag recording on the status indicator
statusIndicator.addEventListener("mousedown", (e) => {
  if (e.button !== 0) return; // Only left click
  e.preventDefault();
  if (!isRecording) {
    startRecording("mouse");
  }
});

window.addEventListener("mouseup", (e) => {
  if (e.button !== 0) return;
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
  const defaultNameBase = (plotTitle || "")
    .trim()
    .replace(/[\/\\?%*:|"<>]/g, "")
    .replace(/\s+/g, "_")
    .slice(0, 80);
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

let resizeRaf = 0;
const plotResizeObserver = new ResizeObserver(() => {
  cancelAnimationFrame(resizeRaf);
  resizeRaf = requestAnimationFrame(layoutCharts);
});
plotResizeObserver.observe(plotDiv);

updateTimerResolutionUI();
renderPlot(true);
