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
const DEFAULT_CSV_FILENAME = "mouseplotter.csv";

const AUTO_PERIOD_LABEL = periodSelect?.options?.[0]?.textContent || "auto";
const rawSupported = "onpointerrawupdate" in window;
const VEL_AXIS_ANCHOR = {
  type: "scatter",
  yaxis: "y2",
  hoverinfo: "skip",
  showlegend: false,
};
const PLOT_BASE_FONT_SIZE = 14;
const PLOT_AXIS_FONT_SIZE = 16;
const PLOT_TITLE_FONT_SIZE = 16;
const PLOT_FONT_FAMILY = "-apple-system, system-ui, sans-serif";
const DEFAULT_PNG_EXPORT_SIZE = { width: 960, height: 720 };

const rootStyle = getComputedStyle(document.documentElement);
const THEME = {
  text: rootStyle.getPropertyValue("--text").trim() || "#0f172a",
  card: rootStyle.getPropertyValue("--card").trim() || "#ffffff",
  border: rootStyle.getPropertyValue("--border").trim() || "rgba(0,0,0,0.15)",
};

function plotLayoutBase(titleText) {
  const layout = {
    font: {
      family: PLOT_FONT_FAMILY,
      size: PLOT_BASE_FONT_SIZE,
      color: THEME.text,
    },
    paper_bgcolor: THEME.card,
    plot_bgcolor: THEME.card,
    margin: {
      l: 56,
      r: 56,
      b: 54,
      t: 34,
      pad: 0,
    },
  };
  if (titleText) {
    layout.title = {
      text: titleText,
      font: { size: PLOT_TITLE_FONT_SIZE },
      x: 0.5,
      xanchor: "center",
      y: 0.98,
      yanchor: "top",
      pad: { t: 0, b: 0, l: 0, r: 0 },
    };
  }
  return layout;
}

let isRecording = false;
let data = new Float64Array(600000); // Flat array [t, x, y, ...]
let index = 0;
let currentEventType = "";
const idleText = (statusIndicator?.textContent || "").trim() ||
  "Click & hold or Space";
let lastCountsToVelocity = NaN;
let isSyncingAxes = false;
let recordingMode = ""; // 'space' or 'mouse'
let plotTitle = "";

function safeNewPlot(div, traces, layout, config) {
  if (!window.Plotly) {
    div.textContent =
      "Plotly failed to load (may be blocked by COEP/CORS). Try self-hosting Plotly or ensure the CDN supports CORS.";
    return null;
  }
  return Plotly.newPlot(div, traces, layout, config);
}

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

function relayout(updates) {
  if (!window.Plotly) return;
  isSyncingAxes = true;
  Plotly.relayout(plotDiv, updates).finally(() => {
    isSyncingAxes = false;
  });
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

function renderPlot(allowEmpty = false) {
  const count = index / 3;
  if (count === 0 && !allowEmpty) return;

  const axisTitleFont = { size: PLOT_AXIS_FONT_SIZE };
  const axisTickFont = { size: PLOT_AXIS_FONT_SIZE };

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
  lastCountsToVelocity = countsToVelocity;
  const countsPerMsToVelocity = (1 / dpiVal) * (25.4 / 1000) * 1000;

  const span = maxCount - minCount;
  const pad = span > 0 ? span * 0.05 : 1;
  const yMin = minCount - pad;
  const yMax = maxCount + pad;

  const RAW_ALPHA = 0.5;
  const SMOOTH_ALPHA = 1.0;

  const mkMarkerTrace = (name, x, y, color, extra = {}) => ({
    x,
    y,
    type: "scattergl",
    mode: "markers",
    name,
    opacity: RAW_ALPHA,
    marker: { size: 3, color },
    ...extra,
  });

  const mkLineTrace = (name, x, y, color, extra = {}) => ({
    x,
    y,
    type: "scattergl",
    mode: "lines",
    name,
    opacity: SMOOTH_ALPHA,
    line: { width: 2.5, color },
    hoverinfo: "y+name",
    ...extra,
  });

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
  const plotInfoText = `${dpiText}<br>${periodLabel}`;

  const layout = {
    ...plotLayoutBase(plotTitle),
    xaxis: {
      title: { text: "", font: axisTitleFont },
      tickfont: axisTickFont,
      zeroline: false,
      showticklabels: false,
      nticks: 12,
      domain: [0, 1],
      anchor: "y",
    },
    xaxis2: {
      title: { text: "time (ms)", font: axisTitleFont },
      tickfont: axisTickFont,
      zeroline: false,
      matches: "x",
      showgrid: true,
      nticks: 12,
      domain: [0, 1],
      anchor: "y3",
    },
    yaxis: {
      title: { text: "counts", font: axisTitleFont },
      tickfont: axisTickFont,
      range: [yMin, yMax],
      domain: [0.35, 1],
      showline: true,
    },
    yaxis2: {
      title: {
        text: "velocity (m/s)",
        font: axisTitleFont,
      },
      tickfont: axisTickFont,
      automargin: true,
      overlaying: "y",
      side: "right",
      range: [yMin * countsToVelocity, yMax * countsToVelocity],
      tickformat: ".2f",
      showgrid: false,
      zeroline: false,
      showline: true,
      anchor: "x",
    },
    yaxis3: {
      title: { text: "Δt (ms)", font: axisTitleFont },
      tickfont: axisTickFont,
      range: [0, 4 * periodMs],
      tickmode: "linear",
      tick0: 0,
      dtick: periodMs,
      domain: [0, 0.3],
      showline: true,
      anchor: "x2",
    },
    legend: {
      x: 0.5,
      y: 0.325,
      xanchor: "center",
      yanchor: "middle",
      orientation: "h",
      valign: "middle",
      bgcolor: THEME.card,
      bordercolor: THEME.border,
      borderwidth: 1,
    },
    annotations: [
      {
        text: plotInfoText,
        xref: "paper",
        yref: "paper",
        x: 0.99,
        y: 0.99,
        xanchor: "right",
        yanchor: "top",
        align: "right",
        showarrow: false,
        font: { size: PLOT_BASE_FONT_SIZE, color: THEME.text },
        bgcolor: THEME.card,
        bordercolor: THEME.border,
        borderwidth: 1,
        borderpad: 4,
      },
    ],
  };

  const yaxis3TicksAuto = {
    "yaxis3.tickmode": "auto",
    "yaxis3.dtick": null,
    "yaxis3.tick0": null,
    "yaxis3.nticks": 7,
  };
  const yaxis3TicksLinear = {
    "yaxis3.tickmode": "linear",
    "yaxis3.tick0": 0,
    "yaxis3.dtick": periodMs,
    "yaxis3.nticks": null,
  };

  const COLOR_X = "#0072B2";
  const COLOR_X_SMOOTH = "#005686";
  const COLOR_Y = "#D55E00";
  const COLOR_Y_SMOOTH = "#A04700";
  const COLOR_DT = "#009E73";
  const COLOR_DT_SMOOTH = "#004F3A";

  // Draw raw markers first, then smoothed lines (on top) for readability.
  // Dummy trace forces Plotly to render the secondary y-axis without duplicating data.
  const traces = [
    mkMarkerTrace("x counts", ts, mx, COLOR_X, { legendrank: 10 }),
    mkMarkerTrace("y counts", ts, my, COLOR_Y, { legendrank: 20 }),
    mkMarkerTrace("Δt raw", dtRawT, dtRaw, COLOR_DT, {
      xaxis: "x2",
      yaxis: "y3",
      legendrank: 30,
    }),
    mkLineTrace("x vel", tSmooth, xVelSmooth, COLOR_X_SMOOTH, {
      yaxis: "y2",
      legendrank: 11,
    }),
    mkLineTrace("y vel", tSmooth, yVelSmooth, COLOR_Y_SMOOTH, {
      yaxis: "y2",
      legendrank: 21,
    }),
    mkLineTrace("Δt smoothed", dtSmoothT, dtSmooth, COLOR_DT_SMOOTH, {
      xaxis: "x2",
      yaxis: "y3",
      legendrank: 31,
    }),
    { x: [], y: [], ...VEL_AXIS_ANCHOR },
  ];

  const p = safeNewPlot(plotDiv, traces, layout, {
    responsive: true,
    // Plotly's modebar "Download plot as a png" uses this.
    toImageButtonOptions: {
      format: "png",
      width: DEFAULT_PNG_EXPORT_SIZE.width,
      height: DEFAULT_PNG_EXPORT_SIZE.height,
      scale: 1,
    },
  });
  if (p && plotDiv?.on) {
    p.then(() => {
      if (plotDiv.removeAllListeners) {
        for (
          const evtName of [
            "plotly_relayout",
            "plotly_doubleclick",
            "plotly_buttonclicked",
          ]
        ) {
          plotDiv.removeAllListeners(evtName);
        }
      }

      plotDiv.on("plotly_doubleclick", () => {
        // After Plotly applies its double-click behavior (toggle between home/autoscale),
        // adjust yaxis3 ticks to avoid excessive gridlines.
        setTimeout(() => {
          if (isSyncingAxes) return;
          const auto = !!plotDiv?.layout?.yaxis3?.autorange;
          relayout(auto ? yaxis3TicksAuto : yaxis3TicksLinear);
        }, 0);
      });

      plotDiv.on("plotly_buttonclicked", (e) => {
        // Keep yaxis3 tick density sane when using modebar controls.
        const btn = e?.button?.name;
        if (btn !== "autoScale2d" && btn !== "resetScale2d") return;
        setTimeout(() => {
          if (isSyncingAxes) return;
          if (btn === "autoScale2d") {
            relayout(yaxis3TicksAuto);
          } else if (btn === "resetScale2d") {
            relayout(yaxis3TicksLinear);
          }
        }, 0);
      });

      plotDiv.on("plotly_relayout", (ev) => {
        if (isSyncingAxes || !ev) return;

        const yaxis3AutoRange = "yaxis3.autorange" in ev
          ? ev["yaxis3.autorange"]
          : undefined;
        const yaxis3RangeChanged = "yaxis3.range" in ev ||
          "yaxis3.range[0]" in ev ||
          "yaxis3.range[1]" in ev;
        const xaxisAutoRange = "xaxis.autorange" in ev
          ? ev["xaxis.autorange"]
          : undefined;
        const xaxisRangeChanged = "xaxis.range" in ev ||
          "xaxis.range[0]" in ev ||
          "xaxis.range[1]" in ev;

        const updates = {};
        const setScaledRange = (targetKey, a0, a1, scale) => {
          if (!Number.isFinite(scale) || scale === 0) return;
          if (!Number.isFinite(a0) || !Number.isFinite(a1)) return;
          updates[targetKey] = [a0 * scale, a1 * scale];
        };

        const isAxisTouched = (axis) =>
          `${axis}.range` in ev ||
          `${axis}.range[0]` in ev ||
          `${axis}.range[1]` in ev ||
          `${axis}.autorange` in ev;

        const getAxisRange = (axis) => {
          const key = `${axis}.range`;
          if (Array.isArray(ev[key]) && ev[key].length === 2) return ev[key];

          const r0Key = `${axis}.range[0]`;
          const r1Key = `${axis}.range[1]`;
          const r0 = r0Key in ev
            ? ev[r0Key]
            : plotDiv?.layout?.[axis]?.range?.[0];
          const r1 = r1Key in ev
            ? ev[r1Key]
            : plotDiv?.layout?.[axis]?.range?.[1];
          return [r0, r1];
        };

        const yaxisChanged = isAxisTouched("yaxis");
        const yaxis2Changed = isAxisTouched("yaxis2");

        // Keep x-grid density sane (especially after autoscale to full range).
        if (xaxisAutoRange === true || xaxisRangeChanged) {
          updates["xaxis.nticks"] = 12;
          updates["xaxis2.nticks"] = 12;
        }

        // If yaxis3 autoranges or is interacted with, allow auto ticks but cap density.
        if (yaxis3AutoRange === true || yaxis3RangeChanged) {
          Object.assign(updates, yaxis3TicksAuto);
        }

        if (yaxisChanged) {
          const [y0, y1] = getAxisRange("yaxis");
          setScaledRange("yaxis2.range", y0, y1, lastCountsToVelocity);
        } else if (yaxis2Changed) {
          const [v0, v1] = getAxisRange("yaxis2");
          setScaledRange("yaxis.range", v0, v1, 1 / lastCountsToVelocity);
        }

        if (Object.keys(updates).length > 0) relayout(updates);
      });
    });
  }
}

window.addEventListener("keydown", (e) => {
  if (e.code !== "Space") return;
  e.preventDefault();
  if (e.repeat) return;
  if (isRecording && recordingMode === "space") {
    stopRecording();
  } else if (!isRecording) {
    startRecording("space");
  }
});

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

updateTimerResolutionUI();
renderPlot(true);
