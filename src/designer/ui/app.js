// NAKA Title Designer Pro - designer UI
// Talks to the native host (designer.cpp) through chrome.webview. The native side renders
// preview frames on the GPU and sends them back as shared buffers (RGBA).
'use strict';

const host = window.chrome && window.chrome.webview;

// ------------------------------------------------------------------ utilities
const $ = (sel) => document.querySelector(sel);
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
const clone = (o) => JSON.parse(JSON.stringify(o));
const newId = (p) => p + Date.now().toString(36) + Math.random().toString(36).slice(2, 6);

function el(tag, attrs, ...children) {
  const e = document.createElement(tag);
  if (attrs) for (const [k, v] of Object.entries(attrs)) {
    if (k === 'class') e.className = v;
    else if (k === 'text') e.textContent = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (v !== undefined && v !== null && v !== false) e.setAttribute(k, v === true ? '' : v);
  }
  for (const c of children.flat()) if (c != null) e.append(c);
  return e;
}

// "#AARRGGBB" <-> { hex: "#RRGGBB", a: 0..100 }
function splitColor(c) {
  c = c || '#FFFFFFFF';
  if (c.length === 7) return { hex: c.toLowerCase(), a: 100 };
  return { hex: '#' + c.slice(3).toLowerCase(), a: Math.round(parseInt(c.slice(1, 3), 16) / 2.55) };
}
function joinColor(hex, a) {
  const aa = Math.round(clamp(a, 0, 100) * 2.55).toString(16).padStart(2, '0');
  return ('#' + aa + hex.slice(1)).toUpperCase();
}
function cssColor(c) {
  const { hex, a } = splitColor(c);
  const n = parseInt(hex.slice(1), 16);
  return `rgba(${n >> 16},${(n >> 8) & 255},${n & 255},${a / 100})`;
}

const LAYER_LABELS = {
  fill: '塗り', stroke: 'フチ', shadow: 'シャドウ', innerShadow: '内側シャドウ',
  longShadow: 'ロングシャドウ', background: '背景（座布団）', extrude: '3D（押し出し）', texture: 'テクスチャ（重ね）',
};
const WEIGHTS = [[100, '100 極細'], [200, '200'], [300, '300 細'], [400, '400 標準'], [500, '500 中'],
  [600, '600'], [700, '700 太'], [800, '800'], [900, '900 極太']];

// Mirrors Light in tdp_doc.h.
const LIGHT_DEFAULTS = { angle: 225, height: 45, intensity: 100, ambient: 35, env: 'soft', motion: 'none', range: 180, period: 4, start: 0, duration: 60 };
// Fill / stroke surface defaults (Layer in tdp_doc.h).
const SURFACE_DEFAULTS = { material: 'none', metallic: 0, roughness: 40, brushed: false, bevel: 'none', bevelWidth: 4, bevelHeight: 4, bumpDepth: 4,
  bevelSoften: 0, bevelDown: false, hiColor: '#FFFFFFFF', hiAmount: 0, shColor: '#FF000000', shAmount: 0, ownLight: false, lightAngle: 225, lightHeight: 45 };
// Mirrors Texture in tdp_doc.h.
const makeTexture = (over) => Object.assign({ path: '', area: 'text', fit: 'cover', scale: 100, offsetX: 0, offsetY: 0, rotation: 0,
  blend: 'normal', opacity: 100, speed: 100, startTime: 0, loop: true }, over);
const BLENDS = [['normal', '通常'], ['multiply', '乗算（暗く重ねる）'], ['screen', 'スクリーン（光・粒子向け）'], ['overlay', 'オーバーレイ'],
  ['add', '加算（光る）'], ['lighten', '比較（明）'], ['darken', '比較（暗）'], ['softLight', 'ソフトライト']];
// [label, material values, colour or null]. Picking one rewrites the layer's paint to the colour.
const SURFACE_PRESETS = [
  ['なし（2D の色）', { material: 'none', metallic: 0, roughness: 40, brushed: false }, null],
  ['光沢', { material: 'lit', metallic: 0, roughness: 20, brushed: false }, null],
  ['金', { material: 'lit', metallic: 100, roughness: 22, brushed: false }, '#FFFFC35A'],
  ['銀', { material: 'lit', metallic: 100, roughness: 25, brushed: false }, '#FFF2F2F0'],
  ['クローム', { material: 'lit', metallic: 100, roughness: 5, brushed: false }, '#FFC8C8CC'],
  ['銅', { material: 'lit', metallic: 100, roughness: 30, brushed: false }, '#FFF0A080'],
  ['ヘアライン', { material: 'lit', metallic: 100, roughness: 28, brushed: true }, '#FFE8E8E8'],
];
// 3D sides: [label, values]
const SIDE_PRESETS = [
  ['標準', { sideColor: '#FF9A9A9A', sideColor2: '#FF3C3C3C', metallic: 0, roughness: 40 }],
  ['金', { sideColor: '#FFE0A040', sideColor2: '#FF6A4410', metallic: 100, roughness: 30 }],
  ['銀', { sideColor: '#FFD8D8D8', sideColor2: '#FF505050', metallic: 100, roughness: 25 }],
  ['クローム', { sideColor: '#FFB0B0B8', sideColor2: '#FF303034', metallic: 100, roughness: 8 }],
  ['銅', { sideColor: '#FFE09070', sideColor2: '#FF5A2A18', metallic: 100, roughness: 30 }],
];

function presetSelect(label, presets, apply) {
  const sel = el('select', { onchange: () => {
    if (sel.value === '') return;
    apply(presets[Number(sel.value)]);
    changed(true);
  } }, el('option', { value: '', text: '選ぶ…' }), presets.map((p, i) => el('option', { value: String(i), text: p[0] })));
  return row(label, sel);
}

// Metal looks best in the studio surroundings: switch to them when a metal preset is picked.
function studioFor(o, metallic) {
  if (metallic > 0) o.light = Object.assign({}, LIGHT_DEFAULTS, o.light, { env: 'studio' });
}

// Bevel + material of a fill / stroke layer.
function surfaceEditor(o, l) {
  for (const [k, v] of Object.entries(SURFACE_DEFAULTS)) if (l[k] === undefined) l[k] = v;
  const parts = [el('div', { class: 'animTitle', text: '質感' }),
    presetSelect('材質プリセット', SURFACE_PRESETS, (p) => {
      Object.assign(l, p[1]);
      if (p[2]) l.paint = { type: 'solid', color: p[2] };
      studioFor(o, p[1].metallic);
    }),
    seg('材質', [['none', 'なし（2D の色）'], ['lit', 'あり']], () => l.material, (v) => { l.material = v; changed(true); })];
  if (l.material === 'lit') {
    parts.push(num('金属っぽさ', () => l.metallic, (v) => { l.metallic = v; }, { unit: '%', min: 0, max: 100 }));
    parts.push(num('粗さ', () => l.roughness, (v) => { l.roughness = v; }, { unit: '%（0 = つるつる）', min: 0, max: 100 }));
    parts.push(check('ヘアライン', () => l.brushed, (v) => { l.brushed = v; }));
  }
  parts.push(seg('ベベル', [['none', 'なし'], ['flat', '平ら'], ['round', '丸み'], ['cove', 'くぼみ']], () => l.bevel, (v) => { l.bevel = v; changed(true); }));
  if (l.bevel !== 'none') {
    parts.push(num('ベベルの幅', () => l.bevelWidth, (v) => { l.bevelWidth = v; }, { unit: 'px', min: 0, step: 0.5 }));
    parts.push(num('ベベルの高さ', () => l.bevelHeight, (v) => { l.bevelHeight = v; }, { unit: 'px', min: 0, step: 0.5 }));
    parts.push(bevelDetails(l));
  }
  // Bump: the brightness of a texture is the height of the surface (lit by the light below).
  l.bump = l.bump || makeTexture({});
  parts.push(el('div', { class: 'animTitle', text: '凹凸（バンプ）' }));
  parts.push(textureEditor(l.bump, { bump: true }));
  if (l.bump.path) parts.push(num('凹凸の高さ', () => l.bumpDepth, (v) => { l.bumpDepth = v; }, { unit: 'px（負で凹む）', step: 0.5 }));
  parts.push(el('div', { class: 'hint', text: '材質「あり」は、この層の色を素の色にして、光と映り込み（「ライトと映り込み」）で描きます。プリセットは色も書き換えます。ベベルはこの層の形の縁に付きます。凹凸は画像の明るさを高さにして、光で陰影を付けます（明るい所が盛り上がる）。' }));
  return el('div', { style: 'display:flex;flex-direction:column;gap:5px' }, ...parts);
}

// Photoshop-style details of a layer's bevel, folded away until needed (opened when something is set).
function bevelDetails(l) {
  const used = l.bevelSoften > 0 || l.bevelDown || l.hiAmount > 0 || l.shAmount > 0 || l.ownLight;
  const d = el('details', { class: 'bevelDetails' }, el('summary', { text: 'ベベルの詳細設定' }));
  if (used || bevelDetailsOpen) d.open = true;
  d.addEventListener('toggle', () => { bevelDetailsOpen = d.open; });
  const own = check('光の向きを個別に決める', () => l.ownLight, (v) => { l.ownLight = v; });
  own.querySelector('input').addEventListener('change', () => buildProps()); // the angle rows appear / disappear
  d.append(
    seg('方向', [[false, '上（盛り上がる）'], [true, '下（へこむ）']], () => !!l.bevelDown, (v) => { l.bevelDown = v; }),
    num('ソフトさ', () => l.bevelSoften || 0, (v) => { l.bevelSoften = v; }, { unit: '（0〜8。大きいほどなめらか）', min: 0, max: 8 }),
    colorRow('ハイライトの色', () => l.hiColor, (c) => { l.hiColor = c; }),
    num('ハイライトの強さ', () => l.hiAmount || 0, (v) => { l.hiAmount = v; }, { unit: '%（0 = なし。光に向く面を明るく）', min: 0, max: 100 }),
    colorRow('シャドウの色', () => l.shColor, (c) => { l.shColor = c; }),
    num('シャドウの強さ', () => l.shAmount || 0, (v) => { l.shAmount = v; }, { unit: '%（0 = なし。光に背く面を暗く）', min: 0, max: 100 }),
    own,
    ...(l.ownLight ? [
      num('光の来る方向', () => l.lightAngle ?? 225, (v) => { l.lightAngle = v; }, { unit: '°（0 = 右, 90 = 下, 225 = 左上）' }),
      num('光の高さ', () => l.lightHeight ?? 45, (v) => { l.lightHeight = v; }, { unit: '°', min: 0, max: 90 })] : []),
    el('div', { class: 'hint', text: 'ハイライトはスクリーン、シャドウは乗算で重ねます。層の「ベベル」（と凹凸）にだけ効き、3D 層のベベルには効きません。「光の向きを個別に」をオンにすると、この層のベベルだけ別の方向から照らします。' }));
  return d;
}
let bevelDetailsOpen = false;

function lightSection(o) {
  const L = o.light = Object.assign({}, LIGHT_DEFAULTS, o.light);
  return section('ライトと映り込み', 'light',
    num('光の来る方向', () => L.angle, (v) => { L.angle = v; }, { unit: '°（0 = 右, 90 = 下, 225 = 左上）' }),
    num('光の高さ', () => L.height, (v) => { L.height = v; }, { unit: '°（90 = 正面から）', min: 0, max: 90 }),
    num('光の強さ', () => L.intensity, (v) => { L.intensity = v; }, { unit: '%', min: 0, max: 300 }),
    num('環境光', () => L.ambient, (v) => { L.ambient = v; }, { unit: '%', min: 0, max: 100 }),
    seg('映り込み', [['soft', 'やわらか'], ['studio', 'スタジオ（金属向け）']], () => L.env, (v) => { L.env = v; }),
    seg('光の動き', [['none', 'なし'], ['orbit', '回る'], ['sweep', '行き来'], ['pass', '一度通る']], () => L.motion, (v) => { L.motion = v; changed(true); }),
    ...(L.motion === 'orbit' ? [num('一周する時間', () => L.period, (v) => { L.period = v; }, { unit: '秒', min: 0.1, step: 0.1 })] : []),
    ...(L.motion === 'sweep' ? [
      num('振れ幅', () => L.range, (v) => { L.range = v; }, { unit: '°（来る方向を中心に左右へ）' }),
      num('1往復の時間', () => L.period, (v) => { L.period = v; }, { unit: '秒', min: 0.1, step: 0.1 })] : []),
    ...(L.motion === 'pass' ? [
      num('通る幅', () => L.range, (v) => { L.range = v; }, { unit: '°（来る方向を中心）' }),
      num('始まり', () => L.start, (v) => { L.start = v; }, { unit: 'フレーム', min: 0 }),
      num('かかる長さ', () => L.duration, (v) => { L.duration = v; }, { unit: 'フレーム', min: 0 })] : []),
    el('div', { class: 'hint', text: '3D 層と、材質・ベベルを使う塗り・フチで共通です。「光の動き」は光の来る方向を時間で動かし、金属や 3D の面に光が走って見えます。' }));
}

function makeLayer(type) {
  const l = { id: newId('L'), type, name: LAYER_LABELS[type], enabled: true, opacity: 100 };
  switch (type) {
    case 'fill': l.paint = { type: 'solid', color: '#FFFFFFFF' }; break;
    case 'stroke': Object.assign(l, { width: 8, position: 'outer', join: 'round', miterLimit: 4, paint: { type: 'solid', color: '#FF000000' } }); break;
    case 'shadow': Object.assign(l, { color: '#B4000000', angle: 135, distance: 8, blur: 8, spread: 0 }); break;
    case 'innerShadow': Object.assign(l, { color: '#96000000', angle: 90, distance: 4, blur: 6, spread: 0 }); break;
    case 'longShadow': Object.assign(l, { angle: 45, length: 24, paint: { type: 'solid', color: '#FF404040' } }); break;
    case 'background': Object.assign(l, { padX: 32, padY: 16, radius: 12, paint: { type: 'solid', color: '#C8000000' } }); break;
    // Mirrors the Layer defaults in tdp_doc.h.
    case 'extrude': Object.assign(l, { depth: 40, sideColor: '#FF9A9A9A', sideColor2: '#FF3C3C3C', bevel: 'round', bevelWidth: 4, bevelHeight: 4,
      rotX: 0, rotY: 0, perspective: 50, metallic: 0, roughness: 40 }); break;
    case 'texture': Object.assign(l, { clip: true, tex: makeTexture({ blend: 'screen' }) }); break;
  }
  return l;
}

function makeTextObject(text, x, y) {
  const d = state.doc.frame;
  const size = Math.round(d.height / 9);
  const stroke = makeLayer('stroke');
  stroke.width = Math.round(size * 0.07);
  return {
    type: 'text', id: newId('T'), name: 'テキスト', visible: true, text: text || 'テロップ',
    x: x ?? d.width / 2, y: y ?? d.height * 0.82, align: 'center', valign: 'middle',
    rotation: 0, scaleX: 100, scaleY: 100, opacity: 100,
    family: 'Yu Gothic UI', size, weight: 700, italic: false, tracking: 0, lineHeight: 0, vertical: false, tcyDigits: 2, tcyChars: '()', paintPerLine: true, rubyScale: 50, rubyGap: 0,
    spans: [], kerning: [], stack: [makeLayer('fill'), stroke, makeLayer('shadow')], light: Object.assign({}, LIGHT_DEFAULTS),
    animIn: { type: 'none' }, animOut: { type: 'none' },
  };
}

// ------------------------------------------------------------------ state
const state = {
  doc: null,
  file: '',
  sel: -1,          // selected object index
  layerSel: null,   // selected layer id
  tool: 'select',
  frame: 0,
  showBg: true,
  showSafe: true,
  bgImage: null,
  fonts: [],
  renderer: '',
  lastMs: 0,
  presets: null,
  presetTab: 'style',
  folderSel: { style: '*', title: '*' }, // '*' = all, '' = not in a folder, else a folder id
  zoom: 1,          // preview zoom relative to "fit to window"
  panX: 0, panY: 0, // preview offset from centred, device px
  dirty: false,
  textSel: [0, 0], // last selection in the text box
  cells: null,     // character cells of the selected text, while the kerning tool is on
  kernSel: null,   // { id, index }: the gap selected with the kerning tool (index = character before it)
};
const history = { stack: [], index: -1, timer: 0 };

const obj = () => (state.doc && state.sel >= 0 ? state.doc.objects[state.sel] : null);
const layerOf = (o) => (o ? o.stack.find((l) => l.id === state.layerSel) : null);

function snapshot() {
  const s = JSON.stringify(state.doc);
  if (history.stack[history.index] === s) return;
  history.stack.length = history.index + 1;
  history.stack.push(s);
  if (history.stack.length > 200) history.stack.shift();
  history.index = history.stack.length - 1;
}
function scheduleSnapshot() {
  clearTimeout(history.timer);
  history.timer = setTimeout(snapshot, 350);
}
function undoRedo(step) {
  clearTimeout(history.timer);
  snapshot();
  const i = history.index + step;
  if (i < 0 || i >= history.stack.length) return;
  history.index = i;
  state.doc = JSON.parse(history.stack[i]);
  if (state.sel >= state.doc.objects.length) state.sel = state.doc.objects.length - 1;
  state.dirty = true;
  buildProps();
  requestRender();
}

// Every edit goes through here. rebuild = the panel layout changed (layer added, type switched, ...).
function changed(rebuild) {
  state.dirty = true;
  // Structural edits get their own undo step; value tweaks are merged while typing / scrubbing.
  if (rebuild) { clearTimeout(history.timer); snapshot(); } else scheduleSnapshot();
  if (rebuild) buildProps();
  requestRender();
}

// ------------------------------------------------------------------ messaging / rendering
function post(msg) { if (host) host.postMessage(msg); }

const render = { pending: false, inFlight: false, seq: 0, thumbs: [], thumbKeys: new Set() };
const frameCanvas = document.createElement('canvas');
let quads = [];

function requestRender() { render.pending = true; pump(); }

function pump() {
  if (render.inFlight || !state.doc) return;
  if (render.pending) {
    render.pending = false;
    render.inFlight = true;
    const v = viewRect();
    const w = Math.max(1, Math.min(Math.round(v.w), state.doc.frame.width));
    const h = Math.max(1, Math.round(w * state.doc.frame.height / state.doc.frame.width));
    // The kerning tool also needs the character cells of the selected text.
    post({ type: 'render', purpose: 'preview', seq: ++render.seq, doc: state.doc, frame: state.frame, width: w, height: h,
      cells: state.tool === 'kern' ? state.sel : -1 });
    return;
  }
  const t = render.thumbs.shift();
  if (t) {
    render.inFlight = true;
    post({ type: 'render', purpose: 'thumb', key: t.key, seq: ++render.seq, doc: t.doc, frame: 100000, width: t.w, height: t.h });
  }
}

function onFrame(info, buffer) {
  render.inFlight = false;
  try {
    const img = new ImageData(new Uint8ClampedArray(buffer.slice(0)), info.width, info.height);
    if (info.purpose === 'thumb') {
      const c = document.createElement('canvas');
      c.width = info.width; c.height = info.height;
      c.getContext('2d').putImageData(img, 0, 0);
      thumbCache.set(info.key, c);
      render.thumbKeys.delete(info.key);
      drawThumb(info.key);
    } else {
      frameCanvas.width = info.width; frameCanvas.height = info.height;
      frameCanvas.getContext('2d').putImageData(img, 0, 0);
      quads = info.quads || [];
      state.cells = info.cells ? { obj: info.cellsObject, cells: info.cells } : null;
      state.lastMs = info.ms;
      draw();
      updateStatus();
    }
  } finally {
    if (window.chrome.webview.releaseBuffer) window.chrome.webview.releaseBuffer(buffer);
  }
  pump();
}

if (host) {
  host.addEventListener('message', (e) => onMessage(e.data));
  host.addEventListener('sharedbufferreceived', (e) => onFrame(e.additionalData, e.getBuffer()));
}

function onMessage(m) {
  switch (m.type) {
    case 'init': init(m); break;
    case 'requestClose': requestClose(); break;
    case 'error': alertDialog(m.message); break;
    case 'saved': onSaved(); break;
    case 'textureInfo': onTextureInfo(m); break;
    case 'texturePicked': {
      const cb = texPicks.get(m.id);
      texPicks.delete(m.id);
      if (cb && m.path) cb(m.path);
      break;
    }
  }
}

function init(m) {
  setupShapeMenu();
  state.doc = m.doc;
  state.file = m.file;
  state.renderer = m.renderer;
  state.fonts = m.fonts || [];
  state.presets = loadPresets(m.presets);
  state.sel = state.doc.objects.length ? 0 : -1;
  if (obj()) state.layerSel = obj().stack[0] && obj().stack[0].id;
  if (m.bgUrl) {
    // The frame may still be rendering when the designer starts: retry until the file exists.
    let tries = 0;
    const loadBg = () => {
      const img = new Image();
      img.onload = () => { state.bgImage = img; draw(); };
      img.onerror = () => { if (++tries < 100) setTimeout(loadBg, 300); };
      img.src = m.bgUrl + '?t=' + Date.now();
    };
    loadBg();
  } else {
    $('#chkBg').checked = false;
    state.showBg = false;
  }
  const slider = $('#frameSlider');
  slider.max = Math.max(0, state.doc.frame.durationFrames - 1);
  state.frame = restFrame();
  slider.value = state.frame;
  updateFrameLabel();
  snapshot();
  buildProps();
  buildPresets();
  resize();
}

// ------------------------------------------------------------------ stage
const view = $('#view');
const ctx = view.getContext('2d');
let checker = null;

const ZOOM_MIN = 0.1, ZOOM_MAX = 16;

// Where the frame sits on the canvas (device px). s = canvas px per doc px.
function viewRect(zoom = state.zoom) {
  const W = view.width, H = view.height, m = 12 * devicePixelRatio;
  if (!state.doc) return { x: 0, y: 0, w: W, h: H, s: 1, fit: 1 };
  const f = state.doc.frame;
  const fit = Math.max(0.01, Math.min((W - 2 * m) / f.width, (H - 2 * m) / f.height));
  const s = fit * zoom;
  const w = f.width * s, h = f.height * s;
  return { x: (W - w) / 2 + state.panX, y: (H - h) / 2 + state.panY, w, h, s, fit };
}
const toView = (v, x, y) => [v.x + x * v.s, v.y + y * v.s];
const toDoc = (v, x, y) => [(x - v.x) / v.s, (y - v.y) / v.s];

// Zoom by `factor` keeping the doc point under (px, py) (device px) where it is.
function zoomAt(px, py, factor) {
  const v = viewRect();
  const z = clamp(state.zoom * factor, ZOOM_MIN, ZOOM_MAX);
  if (z === state.zoom) return;
  const [dx, dy] = toDoc(v, px, py);
  const v2 = viewRect(z);
  state.zoom = z;
  state.panX += px - (v2.x + dx * v2.s);
  state.panY += py - (v2.y + dy * v2.s);
  viewChanged();
}
function stepZoom(factor) { zoomAt(view.width / 2, view.height / 2, factor); }
function fitView() { state.zoom = 1; state.panX = state.panY = 0; viewChanged(); }
function actualSize() {
  const v = viewRect(1);
  state.zoom = clamp(devicePixelRatio / v.fit, ZOOM_MIN, ZOOM_MAX);
  state.panX = state.panY = 0;
  viewChanged();
}
let viewTimer = 0;
function viewChanged() {
  draw();
  updateZoomLabel();
  // The preview is rendered at the on-screen size (up to the frame size): refresh it once zooming settles.
  clearTimeout(viewTimer);
  viewTimer = setTimeout(requestRender, 120);
}
function updateZoomLabel() {
  if (!state.doc) return;
  $('#zoomLabel').textContent = Math.round(viewRect().s / devicePixelRatio * 100) + '%';
}

function resize() {
  const r = $('#stage').getBoundingClientRect();
  view.width = Math.max(1, Math.round(r.width * devicePixelRatio));
  view.height = Math.max(1, Math.round(r.height * devicePixelRatio));
  draw();
  updateZoomLabel();
  requestRender();
}
new ResizeObserver(resize).observe($('#stage'));

// ------------------------------------------------------------------ panel layout
// The right panel and the style list are resized by dragging their splitters; sizes are remembered.
const LAYOUT_DEFAULT = { propsW: 360, stylesH: 150 };
const layout = (() => {
  try { return Object.assign({}, LAYOUT_DEFAULT, JSON.parse(localStorage.getItem('layout') || '{}')); } catch { return Object.assign({}, LAYOUT_DEFAULT); }
})();
// The stage always keeps some room, so a small window shrinks the panels instead of hiding them.
const clampPropsW = (w) => clamp(w, 220, Math.max(220, innerWidth - 44 - 5 - 120));
const clampStylesH = (h) => clamp(h, 70, Math.max(70, Math.round(innerHeight * 0.55)));
function applyLayout() {
  const root = document.documentElement.style;
  root.setProperty('--propsW', clampPropsW(layout.propsW) + 'px');
  root.setProperty('--stylesH', clampStylesH(layout.stylesH) + 'px');
}
function saveLayout() { try { localStorage.setItem('layout', JSON.stringify(layout)); } catch { /* ignore */ } }
function splitter(sel, prop, valueAt, clampFn) {
  const s = $(sel);
  s.addEventListener('pointerdown', (e) => { s.setPointerCapture(e.pointerId); s.classList.add('active'); e.preventDefault(); });
  s.addEventListener('pointermove', (e) => {
    if (!s.classList.contains('active')) return;
    layout[prop] = clampFn(valueAt(e));
    applyLayout();
  });
  const end = () => { if (s.classList.contains('active')) { s.classList.remove('active'); saveLayout(); } };
  s.addEventListener('pointerup', end);
  s.addEventListener('pointercancel', end);
  s.addEventListener('dblclick', () => { layout[prop] = LAYOUT_DEFAULT[prop]; applyLayout(); saveLayout(); });
}
splitter('#splitV', 'propsW', (e) => innerWidth - e.clientX - 2, clampPropsW);
splitter('#splitH', 'stylesH', (e) => innerHeight - e.clientY - 2, clampStylesH);
window.addEventListener('resize', applyLayout);
applyLayout();

function draw() {
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.fillStyle = '#1b1b1b';
  ctx.fillRect(0, 0, view.width, view.height);
  if (!state.doc) return;
  const v = viewRect();
  if (state.showBg && state.bgImage) {
    ctx.drawImage(state.bgImage, v.x, v.y, v.w, v.h);
  } else {
    if (!checker) {
      const c = document.createElement('canvas');
      c.width = c.height = 16;
      const g = c.getContext('2d');
      g.fillStyle = '#6e6e6e'; g.fillRect(0, 0, 16, 16);
      g.fillStyle = '#969696'; g.fillRect(0, 0, 8, 8); g.fillRect(8, 8, 8, 8);
      checker = ctx.createPattern(c, 'repeat');
    }
    ctx.fillStyle = checker;
    ctx.fillRect(v.x, v.y, v.w, v.h);
  }
  if (frameCanvas.width > 1) {
    ctx.imageSmoothingQuality = 'high';
    ctx.drawImage(frameCanvas, v.x, v.y, v.w, v.h);
  }
  ctx.strokeStyle = '#000';
  ctx.lineWidth = 1;
  ctx.strokeRect(v.x - 0.5, v.y - 0.5, v.w + 1, v.h + 1);

  if (state.showSafe) {
    ctx.setLineDash([4 * devicePixelRatio, 4 * devicePixelRatio]);
    ctx.strokeStyle = 'rgba(255,255,255,0.35)';
    for (const k of [0.9, 0.8]) {
      const w = v.w * k, h = v.h * k;
      ctx.strokeRect(v.x + (v.w - w) / 2, v.y + (v.h - h) / 2, w, h);
    }
    ctx.setLineDash([]);
  }

  const q = quads[state.sel];
  if (q && q.length === 8) {
    ctx.strokeStyle = '#2d8ceb';
    ctx.lineWidth = 1.5 * devicePixelRatio;
    ctx.beginPath();
    for (let i = 0; i < 4; i++) {
      const [x, y] = toView(v, q[i * 2], q[i * 2 + 1]);
      if (i) ctx.lineTo(x, y); else ctx.moveTo(x, y);
    }
    ctx.closePath();
    ctx.stroke();
    ctx.fillStyle = '#fff';
    const hs = 3.5 * devicePixelRatio;
    for (let i = 0; i < 4; i++) {
      const [x, y] = toView(v, q[i * 2], q[i * 2 + 1]);
      ctx.fillRect(x - hs, y - hs, hs * 2, hs * 2);
      ctx.strokeRect(x - hs, y - hs, hs * 2, hs * 2);
    }
    const o = obj();
    if (o) {
      const [ax, ay] = toView(v, o.x, o.y);
      ctx.beginPath();
      ctx.arc(ax, ay, 4 * devicePixelRatio, 0, Math.PI * 2);
      ctx.stroke();
    }
  }
  if (state.tool === 'kern') drawKernHandles(v);
}

// ---- kerning tool: handles between the characters of the selected text
// Gaps in reading order. index = the character before the gap (what `kerning` is keyed by);
// line = the cell edge in doc px; dir = unit vector along the line of text (view axes), em = font size there.
function kernGaps(o) {
  const c = state.cells && state.cells.obj === state.sel ? state.cells.cells : null;
  if (!c) return [];
  const sizes = charStyles(o);
  const out = [];
  for (let i = 0; i + 1 < c.length; i++) {
    const a = c[i], b = c[i + 1];
    if (b.ch <= a.ch || b.ch - 1 >= o.text.length) continue;
    if (/[\n\r]/.test(o.text.slice(a.ch, b.ch))) continue;
    const q = a.q;
    const line = o.vertical ? [q[6], q[7], q[4], q[5]] : [q[2], q[3], q[4], q[5]];
    const d = o.vertical ? [q[6] - q[0], q[7] - q[1]] : [q[2] - q[0], q[3] - q[1]];
    const len = Math.hypot(d[0], d[1]) || 1;
    out.push({ index: b.ch - 1, line, dir: [d[0] / len, d[1] / len], em: (sizes[a.ch] && sizes[a.ch].size) || o.size,
      pair: o.text.slice(a.ch, b.ch) + (o.text[b.ch] || '') });
  }
  return out;
}

function drawKernHandles(v) {
  const o = obj();
  if (!o) return;
  const kern = kernArray(o);
  ctx.save();
  for (const g of kernGaps(o)) {
    const [x1, y1] = toView(v, g.line[0], g.line[1]), [x2, y2] = toView(v, g.line[2], g.line[3]);
    const sel = state.kernSel && state.kernSel.id === o.id && state.kernSel.index === g.index;
    ctx.strokeStyle = sel ? '#ff9a2d' : kern[g.index] ? 'rgba(120,220,140,0.9)' : 'rgba(80,170,255,0.7)';
    ctx.lineWidth = (sel ? 3 : 1.5) * devicePixelRatio;
    ctx.beginPath();
    ctx.moveTo(x1, y1);
    ctx.lineTo(x2, y2);
    ctx.stroke();
    if (sel) {
      const value = kern[g.index] || 0;
      const label = `カーニング ${value >= 0 ? '+' : ''}${Math.round(value / g.em * 1000)}`;
      ctx.font = `${12 * devicePixelRatio}px "Yu Gothic UI", sans-serif`;
      const w = ctx.measureText(label).width + 10 * devicePixelRatio;
      const lx = Math.min(x1, x2) - w / 2 + Math.abs(x2 - x1) / 2, ly = Math.min(y1, y2) - 8 * devicePixelRatio;
      ctx.fillStyle = 'rgba(0,0,0,0.75)';
      ctx.fillRect(lx, ly - 15 * devicePixelRatio, w, 18 * devicePixelRatio);
      ctx.fillStyle = '#ffb35c';
      ctx.fillText(label, lx + 5 * devicePixelRatio, ly - 1.5 * devicePixelRatio);
    }
  }
  ctx.restore();
}

// The gap nearest to a point (device px on the canvas), if it is close enough.
function hitGap(o, px, py) {
  const v = viewRect();
  let best = null, bestD = 10 * devicePixelRatio;
  for (const g of kernGaps(o)) {
    const [x1, y1] = toView(v, g.line[0], g.line[1]), [x2, y2] = toView(v, g.line[2], g.line[3]);
    const dx = x2 - x1, dy = y2 - y1, l2 = dx * dx + dy * dy || 1;
    const t = clamp(((px - x1) * dx + (py - y1) * dy) / l2, 0, 1);
    const d = Math.hypot(px - (x1 + t * dx), py - (y1 + t * dy));
    if (d < bestD) { bestD = d; best = g; }
  }
  return best;
}

// Changes the kerning after character `index` by delta px.
function kernAdjust(o, index, delta) {
  const kern = kernArray(o);
  if (index < 0 || index >= kern.length) return;
  kern[index] = Math.round((kern[index] + delta) * 100) / 100;
  writeKern(o, kern);
  changed(false);
  refreshRange();
}

// Step of the arrow keys: 20 / 1000 em (Shift 100, Alt 5), like After Effects.
function kernStep(o, index, e) {
  const st = charStyles(o)[index];
  const em = (st && st.size) || o.size;
  return em * (e.shiftKey ? 0.1 : e.altKey ? 0.005 : 0.02);
}

// Moves the selected gap to the next / previous one in the text.
function kernMove(o, dir) {
  const gaps = kernGaps(o);
  if (!gaps.length) return;
  const cur = state.kernSel ? gaps.findIndex((g) => g.index === state.kernSel.index) : -1;
  const next = clamp(cur < 0 ? (dir > 0 ? 0 : gaps.length - 1) : cur + dir, 0, gaps.length - 1);
  state.kernSel = { id: o.id, index: gaps[next].index };
  draw();
}

function pointInQuad(q, x, y) {
  if (!q || q.length !== 8) return false;
  let inside = false;
  for (let i = 0, j = 3; i < 4; j = i++) {
    const xi = q[i * 2], yi = q[i * 2 + 1], xj = q[j * 2], yj = q[j * 2 + 1];
    if ((yi > y) !== (yj > y) && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi) inside = !inside;
  }
  return inside;
}

let drag = null;
let pan = null;
let kernDrag = null;
let resizeDrag = null;
function startPan(e) {
  pan = { x: e.clientX, y: e.clientY, px: state.panX, py: state.panY };
  view.setPointerCapture(e.pointerId);
  view.classList.add('panning');
}
// Mouse wheel zooms around the pointer (Ctrl is not needed; trackpad pinch arrives as Ctrl + wheel).
view.addEventListener('wheel', (e) => {
  if (!state.doc) return;
  e.preventDefault();
  const unit = e.deltaMode === 1 ? 33 : e.deltaMode === 2 ? 400 : 1;
  zoomAt(e.offsetX * devicePixelRatio, e.offsetY * devicePixelRatio, Math.exp(-e.deltaY * unit * 0.0015));
}, { passive: false });
view.addEventListener('mousedown', (e) => { if (e.button === 1) e.preventDefault(); }); // no middle-click autoscroll
view.addEventListener('pointerdown', (e) => {
  if (!state.doc) return;
  view.focus();
  // Middle button or Alt + drag moves the view.
  if (e.button === 1 || (e.button === 0 && e.altKey)) { e.preventDefault(); startPan(e); return; }
  if (e.button !== 0) return;
  const v = viewRect();
  const [x, y] = toDoc(v, e.offsetX * devicePixelRatio, e.offsetY * devicePixelRatio);
  if (state.tool === 'text') {
    const o = makeTextObject('テキスト', Math.round(x), Math.round(y));
    state.doc.objects.push(o);
    selectObject(state.doc.objects.length - 1);
    setTool('select');
    changed(true);
    focusText(true);
    return;
  }
  if (state.tool === 'kern') {
    // Click a gap between two characters to select it; drag along the line (or use the arrow keys) to change it.
    const ko = obj();
    const g = ko ? hitGap(ko, e.offsetX * devicePixelRatio, e.offsetY * devicePixelRatio) : null;
    if (g) {
      state.kernSel = { id: ko.id, index: g.index };
      kernDrag = { x: e.clientX, y: e.clientY, start: kernArray(ko)[g.index], dir: g.dir, scale: (ko.vertical ? ko.scaleY : ko.scaleX) / 100 || 1 };
      view.setPointerCapture(e.pointerId);
      draw();
      return;
    }
    state.kernSel = null;
  }
  // A shape's corner squares resize it.
  const rq = quads[state.sel];
  if (state.tool === 'select' && isShape(obj()) && rq && rq.length === 8) {
    const rv = viewRect(), px = e.offsetX * devicePixelRatio, py = e.offsetY * devicePixelRatio;
    for (let i = 0; i < 4; i++) {
      const [cx, cy] = toView(rv, rq[i * 2], rq[i * 2 + 1]);
      if (Math.hypot(px - cx, py - cy) < 9 * devicePixelRatio) {
        resizeDrag = { w0: obj().shape.width, h0: obj().shape.height };
        view.setPointerCapture(e.pointerId);
        return;
      }
    }
  }
  let hit = -1;
  for (let i = state.doc.objects.length - 1; i >= 0; i--) {
    if (state.doc.objects[i].visible !== false && pointInQuad(quads[i], x, y)) { hit = i; break; }
  }
  if (hit !== state.sel) { selectObject(hit); buildProps(); if (state.tool === 'kern') requestRender(); draw(); }
  const o = obj();
  if (o && hit >= 0 && state.tool !== 'kern') {
    drag = { x, y, ox: o.x, oy: o.y, moved: false };
    view.setPointerCapture(e.pointerId);
  } else if (hit < 0 && (state.zoom !== 1 || state.panX || state.panY)) {
    startPan(e); // dragging empty space moves a zoomed view
  }
});
view.addEventListener('pointermove', (e) => {
  if (pan) {
    state.panX = pan.px + (e.clientX - pan.x) * devicePixelRatio;
    state.panY = pan.py + (e.clientY - pan.y) * devicePixelRatio;
    draw();
    return;
  }
  if (resizeDrag) {
    const so = obj();
    if (!so || !isShape(so)) return;
    const [x, y] = toDoc(viewRect(), e.offsetX * devicePixelRatio, e.offsetY * devicePixelRatio);
    const a = -so.rotation * Math.PI / 180, dx = x - so.x, dy = y - so.y;
    const lx = (dx * Math.cos(a) - dy * Math.sin(a)) / ((so.scaleX || 100) / 100);
    const ly = (dx * Math.sin(a) + dy * Math.cos(a)) / ((so.scaleY || 100) / 100);
    let w = Math.max(4, Math.round(Math.abs(lx) * 2)), h = Math.max(4, Math.round(Math.abs(ly) * 2));
    if (e.shiftKey) { // keep the proportions
      const r = resizeDrag.w0 / resizeDrag.h0;
      if (w / h > r) h = Math.round(w / r); else w = Math.round(h * r);
    }
    so.shape.width = w;
    so.shape.height = h;
    changed(false);
    return;
  }
  if (kernDrag) {
    const ko = obj();
    if (!ko || !state.kernSel) return;
    const v = viewRect();
    const along = ((e.clientX - kernDrag.x) * kernDrag.dir[0] + (e.clientY - kernDrag.y) * kernDrag.dir[1]) * devicePixelRatio;
    const kern = kernArray(ko);
    kern[state.kernSel.index] = Math.round((kernDrag.start + along / v.s / kernDrag.scale) * 2) / 2;
    writeKern(ko, kern);
    changed(false);
    refreshRange();
    return;
  }
  if (!drag) return;
  const v = viewRect();
  const [x, y] = toDoc(v, e.offsetX * devicePixelRatio, e.offsetY * devicePixelRatio);
  let dx = x - drag.x, dy = y - drag.y;
  if (e.shiftKey) { if (Math.abs(dx) > Math.abs(dy)) dy = 0; else dx = 0; }
  const o = obj();
  o.x = Math.round(drag.ox + dx);
  o.y = Math.round(drag.oy + dy);
  drag.moved = true;
  syncTransformInputs();
  changed(false);
});
const endDrags = () => {
  const resized = !!resizeDrag;
  drag = null; pan = null; kernDrag = null; resizeDrag = null;
  view.classList.remove('panning');
  if (resized) buildProps(); // the width / height fields follow the new size
};
view.addEventListener('pointerup', endDrags);
view.addEventListener('pointercancel', endDrags);
view.addEventListener('dblclick', () => { if (obj()) focusText(false); });
view.tabIndex = 0;

function selectObject(i) {
  state.sel = i;
  const o = obj();
  state.layerSel = o && o.stack.length ? (o.stack.find((l) => l.id === state.layerSel) || o.stack[0]).id : null;
  state.textSel = [0, 0];
  state.kernSel = null;
  state.cells = null;
}

function focusText(selectAll) {
  const ta = $('#textInput');
  if (!ta) return;
  ta.focus();
  if (selectAll) ta.select();
}

// ------------------------------------------------------------------ toolbar
function setTool(t) {
  const was = state.tool;
  state.tool = t;
  document.querySelectorAll('.tool').forEach((b) => b.classList.toggle('active', b.dataset.tool === t));
  view.style.cursor = t === 'text' ? 'text' : t === 'kern' ? 'col-resize' : 'default';
  if (t !== 'kern') { state.kernSel = null; state.cells = null; }
  if (t === 'kern' || was === 'kern') requestRender(); // the host sends the character cells only for this tool
  draw();
}
document.querySelectorAll('.tool').forEach((b) => b.addEventListener('click', () => setTool(b.dataset.tool)));

// The "add shape" menu is filled when the document arrives (SHAPE_LABELS is defined further down).
function setupShapeMenu() {
  const sel = $('#selAddShape');
  if (sel.options.length > 1) return;
  for (const [k, t] of Object.entries(SHAPE_LABELS)) sel.append(el('option', { value: k, text: t }));
  sel.onchange = () => {
    if (!sel.value) return;
    state.doc.objects.push(makeShapeObject(sel.value));
    selectObject(state.doc.objects.length - 1);
    sel.value = '';
    changed(true);
  };
}
$('#btnAddText').onclick = () => {
  state.doc.objects.push(makeTextObject());
  selectObject(state.doc.objects.length - 1);
  changed(true);
  focusText(true);
};
$('#btnDupObj').onclick = () => {
  const o = obj();
  if (!o) return;
  const c = clone(o);
  c.id = newId('T');
  c.name = o.name + ' のコピー';
  c.x += 20; c.y += 20;
  state.doc.objects.splice(state.sel + 1, 0, c);
  selectObject(state.sel + 1);
  changed(true);
};
$('#btnDelObj').onclick = deleteObject;
function deleteObject() {
  if (!obj()) return;
  state.doc.objects.splice(state.sel, 1);
  selectObject(Math.min(state.sel, state.doc.objects.length - 1));
  changed(true);
}
$('#btnUndo').onclick = () => undoRedo(-1);
$('#btnRedo').onclick = () => undoRedo(1);
document.querySelectorAll('[data-align]').forEach((b) => b.addEventListener('click', () => {
  const o = obj();
  if (!o) return;
  const f = state.doc.frame;
  // In vertical text align is the vertical anchor and valign the horizontal one.
  if (b.dataset.align === 'h') { if (o.vertical) o.valign = 'middle'; else o.align = 'center'; o.x = f.width / 2; }
  if (b.dataset.align === 'v') { if (o.vertical) o.align = 'center'; else o.valign = 'middle'; o.y = f.height / 2; }
  if (b.dataset.align === 'lower') { o.align = 'center'; o.valign = 'middle'; o.x = f.width / 2; o.y = Math.round(f.height * 0.82); }
  changed(true);
}));
$('#chkBg').onchange = (e) => { state.showBg = e.target.checked; draw(); };
$('#chkSafe').onchange = (e) => { state.showSafe = e.target.checked; draw(); };
$('#btnFit').onclick = fitView;
$('#zoomLabel').onclick = actualSize;
$('#btnZoomIn').onclick = () => stepZoom(1.25);
$('#btnZoomOut').onclick = () => stepZoom(1 / 1.25);
$('#frameSlider').oninput =(e) => { stopPlay(); state.frame = Number(e.target.value); updateFrameLabel(); requestRender(); };
function updateFrameLabel() {
  $('#frameLabel').textContent = `${state.frame} / ${state.doc ? state.doc.frame.durationFrames : 0} f`;
}
$('#btnSave').onclick = save;
$('#btnSaveOnly').onclick = saveOnly;
$('#btnCancel').onclick = requestClose;

// Save and close (the importer re-reads the title when the designer exits).
function save() {
  clearTimeout(history.timer);
  post({ type: 'save', doc: state.doc });
}
// Save and stay (Ctrl+S). The host answers "saved".
function saveOnly() {
  clearTimeout(history.timer);
  post({ type: 'saveOnly', doc: state.doc });
}
function onSaved() {
  state.dirty = false;
  const s = $('#statusRight');
  s.textContent = '保存しました';
  clearTimeout(onSaved.timer);
  onSaved.timer = setTimeout(updateStatus, 2000);
}

// Window close (x) / Cancel button: with unsaved changes ask what to do.
let closing = false;
async function requestClose() {
  if (closing) return;
  closing = true;
  try {
    if (state.dirty) {
      const r = await choiceDialog('変更が保存されていません。', [
        { label: '保存して閉じる', value: 'save', primary: true },
        { label: '保存せずに閉じる', value: 'discard' },
        { label: 'キャンセル', value: 'cancel' },
      ]);
      if (r === 'save') { save(); return; }
      if (r !== 'discard') return;
    }
    post({ type: 'cancel' });
  } finally {
    closing = false;
  }
}

document.addEventListener('keydown', (e) => {
  const typing = /^(INPUT|TEXTAREA|SELECT)$/.test(document.activeElement.tagName);
  if (e.ctrlKey && e.key.toLowerCase() === 's') { e.preventDefault(); saveOnly(); return; }
  if (e.ctrlKey && !typing && e.key.toLowerCase() === 'z') { e.preventDefault(); undoRedo(e.shiftKey ? 1 : -1); return; }
  if (e.ctrlKey && !typing && e.key.toLowerCase() === 'y') { e.preventDefault(); undoRedo(1); return; }
  if (e.key === 'F12') { post({ type: 'devtools' }); return; }
  if (e.ctrlKey && e.key === '0') { e.preventDefault(); fitView(); return; }
  if (e.ctrlKey && e.key === '1') { e.preventDefault(); actualSize(); return; }
  if (e.ctrlKey && (e.key === '+' || e.key === '=')) { e.preventDefault(); stepZoom(1.25); return; }
  if (e.ctrlKey && (e.key === '-' || e.key === '_')) { e.preventDefault(); stepZoom(1 / 1.25); return; }
  // Alt + arrows in the text box change the kerning at the caret (After Effects style; Shift = bigger steps).
  if (typing && document.activeElement.id === 'textInput' && e.altKey && /^Arrow(Left|Right|Up|Down)$/.test(e.key)) {
    const o = obj(), ta = document.activeElement, a = ta.selectionStart;
    e.preventDefault();
    if (o && a === ta.selectionEnd && a > 0 && a < o.text.length) {
      const widen = e.key === 'ArrowRight' || e.key === 'ArrowDown';
      kernAdjust(o, a - 1, (widen ? 1 : -1) * kernStep(o, a - 1, { shiftKey: e.shiftKey, altKey: false }));
      state.kernSel = { id: o.id, index: a - 1 };
    }
    return;
  }
  if (typing) return;
  // Kerning tool with a gap selected: arrows widen / narrow it, Ctrl+arrows or Tab move to the next gap.
  if (state.tool === 'kern' && state.kernSel) {
    const ko = obj();
    if (ko && ko.id === state.kernSel.id) {
      const plus = ko.vertical ? 'ArrowDown' : 'ArrowRight', minus = ko.vertical ? 'ArrowUp' : 'ArrowLeft';
      if (e.key === plus || e.key === minus) {
        e.preventDefault();
        if (e.ctrlKey) kernMove(ko, e.key === plus ? 1 : -1);
        else kernAdjust(ko, state.kernSel.index, (e.key === plus ? 1 : -1) * kernStep(ko, state.kernSel.index, e));
        return;
      }
      if (e.key === 'Tab') { e.preventDefault(); kernMove(ko, e.shiftKey ? -1 : 1); return; }
      if (e.key === 'Escape') { state.kernSel = null; draw(); return; }
      if (e.key === 'Delete' || e.key === 'Backspace') { // back to the font's own spacing
        e.preventDefault();
        const kern = kernArray(ko);
        kern[state.kernSel.index] = 0;
        writeKern(ko, kern);
        changed(false);
        refreshRange();
        return;
      }
      if (e.key.startsWith('Arrow')) { e.preventDefault(); return; }
    }
  }
  if (e.ctrlKey && e.key.toLowerCase() === 'd') { e.preventDefault(); $('#btnDupObj').click(); return; }
  if (e.key === ' ') { e.preventDefault(); togglePlay(); return; }
  if (e.key === 'v' || e.key === 'V') setTool('select');
  if (e.key === 't' || e.key === 'T') setTool('text');
  if (e.key === 'k' || e.key === 'K') setTool('kern');
  if (e.key === 'Delete') deleteObject();
  const o = obj();
  if (o && e.key.startsWith('Arrow')) {
    const d = e.shiftKey ? 10 : 1;
    if (e.key === 'ArrowLeft') o.x -= d;
    if (e.key === 'ArrowRight') o.x += d;
    if (e.key === 'ArrowUp') o.y -= d;
    if (e.key === 'ArrowDown') o.y += d;
    e.preventDefault();
    syncTransformInputs();
    changed(false);
  }
});

function updateStatus() {
  const o = obj();
  $('#statusLeft').textContent = o ? `${o.name}　X ${Math.round(o.x)}  Y ${Math.round(o.y)}` : '';
  $('#statusRight').textContent = `${state.renderer}　描画 ${state.lastMs.toFixed(1)} ms`;
}

// ------------------------------------------------------------------ property widgets
const collapsed = (() => { try { return JSON.parse(localStorage.getItem('collapsed') || '{}'); } catch { return {}; } })();

function section(title, key, ...content) {
  const s = el('section', { class: 'section' + (collapsed[key] ? ' collapsed' : '') },
    el('h3', { text: title, onclick: () => {
      s.classList.toggle('collapsed');
      collapsed[key] = s.classList.contains('collapsed');
      try { localStorage.setItem('collapsed', JSON.stringify(collapsed)); } catch { /* ignore */ }
    } }),
    el('div', { class: 'body' }, ...content));
  return s;
}

function row(label, ...content) {
  return el('div', { class: 'row' }, label != null ? el('label', { class: 'name', text: label }) : null, ...content);
}

// Number input with a draggable label (scrub left / right like Adobe apps).
function num(label, get, set, opt = {}) {
  const step = opt.step ?? 1;
  const input = el('input', { type: 'number', step, min: opt.min, max: opt.max, value: round(get()) });
  const apply = (v, commitRebuild) => {
    if (Number.isNaN(v)) return;
    if (opt.min != null) v = Math.max(opt.min, v);
    if (opt.max != null) v = Math.min(opt.max, v);
    set(v);
    input.value = round(v);
    changed(commitRebuild);
  };
  input.addEventListener('input', () => { if (input.value !== '') apply(Number(input.value), false); });
  const r = row(label, input, opt.unit ? el('span', { class: 'unit', text: opt.unit }) : null);
  const lab = r.querySelector('label');
  if (lab) {
    lab.classList.add('scrub');
    lab.addEventListener('pointerdown', (e) => {
      const start = Number(get()) || 0, x0 = e.clientX;
      lab.setPointerCapture(e.pointerId);
      const move = (ev) => apply(start + Math.round((ev.clientX - x0) / 2) * step * (ev.shiftKey ? 10 : 1), false);
      const up = () => { lab.removeEventListener('pointermove', move); lab.removeEventListener('pointerup', up); };
      lab.addEventListener('pointermove', move);
      lab.addEventListener('pointerup', up);
    });
  }
  input.dataset.key = opt.key || '';
  return r;
  function round(v) { return Math.round(Number(v) * 100) / 100; }
}

function colorInputs(get, set) {
  const c = splitColor(get());
  const picker = el('input', { type: 'color', value: c.hex });
  const alpha = el('input', { type: 'number', min: 0, max: 100, value: c.a, title: '不透明度 %' });
  const apply = () => { set(joinColor(picker.value, Number(alpha.value) || 0)); changed(false); };
  picker.addEventListener('input', apply);
  alpha.addEventListener('input', apply);
  return [picker, alpha, el('span', { class: 'unit', text: '%' })];
}
const colorRow = (label, get, set) => row(label, ...colorInputs(get, set));

function select(label, options, get, set, rebuild) {
  const s = el('select', { onchange: () => { set(s.value); changed(rebuild); } },
    options.map(([v, t]) => el('option', { value: v, text: t })));
  s.value = String(get());
  return row(label, s);
}

function seg(label, options, get, set) {
  const wrap = el('div', { class: 'seg' });
  for (const [v, t] of options) {
    wrap.append(el('button', { class: get() === v ? 'on' : '', text: t, onclick: () => {
      set(v);
      wrap.querySelectorAll('button').forEach((b) => b.classList.toggle('on', b.textContent === t));
      changed(false);
    } }));
  }
  return row(label, wrap);
}

function check(label, get, set) {
  const c = el('input', { type: 'checkbox' });
  c.checked = !!get();
  c.onchange = () => { set(c.checked); changed(false); };
  return row(label, c);
}

// ------------------------------------------------------------------ textures (images / videos with alpha)
const texInfo = new Map();   // path -> last "textureInfo" message from the host
const texPicks = new Map();  // request id -> callback of a file dialog
const texPoll = new Map();   // path -> timer while a video is being converted

const BUILTIN_NOISE = [['grain', '粒状（細かいざらつき）'], ['rough', 'ざらつき（粗い）'], ['cloud', '雲（ムラ）'], ['fiber', '繊維（ヘアライン）']];
const isBuiltin = (path) => !!path && path.startsWith('builtin:');
const texLabel = (path) => {
  if (!path) return '（未選択）';
  if (isBuiltin(path)) return 'ノイズ：' + (BUILTIN_NOISE.find(([k]) => 'builtin:' + k === path) || [0, path])[1];
  return path.split(/[\\/]/).pop();
};

function texStatusText(path) {
  if (!path) return 'PNG・JPG などの画像、MOV・MP4 などの動画（透過動画も可）、または内蔵のノイズを選べます。';
  if (isBuiltin(path)) return '内蔵ノイズ（ファイル不要・タイルで繰り返し）';
  const i = texInfo.get(path);
  if (!i) return '確認中…';
  if (i.error) return i.error;
  if (i.converting || i.needsConversion) return `動画を変換中… ${i.done || 0} フレーム（初回だけ。ffmpeg を使います）`;
  if (!i.ok) return '読み込めません';
  return i.video ? `動画 ${i.width}×${i.height}・${i.frames} フレーム・${(+i.fps).toFixed(2)} fps` : `画像 ${i.width}×${i.height}`;
}

// prepare: also start the conversion of a video that has none yet.
function requestTexInfo(path, prepare) {
  if (path) post({ type: prepare ? 'prepareTexture' : 'textureInfo', path });
}

function onTextureInfo(m) {
  const prev = texInfo.get(m.path);
  texInfo.set(m.path, m);
  document.querySelectorAll('.texStatus').forEach((e) => {
    if (e.dataset.path === m.path) { e.textContent = texStatusText(m.path); e.classList.toggle('bad', !!m.error); }
  });
  clearTimeout(texPoll.get(m.path));
  if (m.converting || m.needsConversion) texPoll.set(m.path, setTimeout(() => requestTexInfo(m.path, true), 600));
  if (!prev || prev.video !== m.video) buildProps(); // video controls appear once we know it is a video
  if (m.ok && !(prev && prev.ok)) requestRender();    // became ready: show it
}

function pickTexture(cb) {
  const id = newId('P');
  texPicks.set(id, cb);
  post({ type: 'pickTexture', id });
}

// Editor of a Texture object (mutated in place). opt.blend / opt.opacity: show those.
function textureEditor(tex, opt = {}) {
  const box = el('div', { class: 'paint texBox' });
  const path = tex.path || '';
  if (path && !texInfo.has(path)) requestTexInfo(path, true);
  const info = texInfo.get(path);
  box.append(row('ファイル', el('span', { class: 'texName', text: texLabel(path), title: path }),
    el('button', { class: 'small', text: path ? '変更…' : '選ぶ…', onclick: () => pickTexture((p) => { tex.path = p; requestTexInfo(p, true); changed(true); }) }),
    path ? el('button', { class: 'small', text: '外す', onclick: () => { tex.path = ''; changed(true); } }) : null));
  // Built-in noise (grain, roughness, ...): picking one also sets what usually suits it (tiled, overlaid).
  const noiseSel = el('select', { onchange: () => {
    if (!noiseSel.value) return;
    tex.path = 'builtin:' + noiseSel.value;
    tex.fit = 'tile';
    tex.scale = 100;
    if (opt.blend) { tex.blend = 'overlay'; tex.opacity = 60; }
    changed(true);
  } }, el('option', { value: '', text: '内蔵ノイズを使う…' }), BUILTIN_NOISE.map(([k, t]) => el('option', { value: k, text: t })));
  box.append(row('ノイズ', noiseSel));
  box.append(el('div', { class: 'hint texStatus', 'data-path': path, text: texStatusText(path) }));
  if (!path) return box;
  box.append(seg('貼る範囲', [['text', 'テキスト（一緒に動く）'], ['frame', '画面全体']], () => tex.area, (v) => { tex.area = v; }));
  box.append(select('合わせ方', [['cover', '全体を覆う'], ['contain', '全体を収める'], ['stretch', '引き伸ばす'], ['tile', '並べる（タイル）']], () => tex.fit, (v) => { tex.fit = v; }));
  box.append(num('大きさ', () => tex.scale, (v) => { tex.scale = v; }, { unit: '%', min: 1 }));
  box.append(num('ずらし X', () => tex.offsetX, (v) => { tex.offsetX = v; }, { unit: 'px' }));
  box.append(num('ずらし Y', () => tex.offsetY, (v) => { tex.offsetY = v; }, { unit: 'px' }));
  box.append(num('回転', () => tex.rotation, (v) => { tex.rotation = v; }, { unit: '°', step: 0.5 }));
  if (opt.blend) box.append(select('合成', BLENDS, () => tex.blend, (v) => { tex.blend = v; }));
  if (opt.opacity) box.append(num('不透明度', () => tex.opacity, (v) => { tex.opacity = v; }, { unit: '%', min: 0, max: 100 }));
  if (info && info.video) {
    box.append(num('再生速度', () => tex.speed, (v) => { tex.speed = v; }, { unit: '%', min: 1 }));
    box.append(num('開始位置', () => tex.startTime, (v) => { tex.startTime = v; }, { unit: '秒（クリップの頭での位置）', min: 0, step: 0.1 }));
    box.append(check('繰り返す', () => tex.loop !== false, (v) => { tex.loop = v; }));
  }
  return box;
}

function paintSwatch(p) {
  if (p.type === 'texture') return p.base ? paintSwatch(p.base) : '#00000000';
  if (p.type === 'solid') return p.color;
  return p.stops ? p.stops[0].color : p.color;
}

// Paint editor: mutates the paint object in place. opt.noTexture: leave out the texture type.
function paintEditor(paint, onTypeChange, opt = {}) {
  const box = el('div', { class: 'paint' });
  const build = () => {
    box.replaceChildren();
    const types = [['solid', '単色'], ['linear', '線形グラデーション'], ['radial', '円形グラデーション'], ['stripe', 'ストライプ'], ['dots', 'ドット']];
    if (!opt.noTexture) types.push(['texture', 'テクスチャ（画像・動画）']);
    const typeSel = el('select', { onchange: () => {
      setPaintType(paint, typeSel.value);
      build();
      changed(false);
      if (onTypeChange) onTypeChange();
    } }, types.map(([v, t]) => el('option', { value: v, text: t })));
    typeSel.value = paint.type;
    box.append(row('塗りの種類', typeSel));
    if (paint.type === 'texture') {
      // A base paint (gradient, ...) under the texture, e.g. a gradient with a transparent video over it.
      box.append(check('下地を敷く', () => !!paint.base, (v) => {
        if (v) paint.base = paint.base || { type: 'solid', color: '#FFFFFFFF' }; else delete paint.base;
        build();
      }));
      if (paint.base) box.append(paintEditor(paint.base, null, { noTexture: true }));
      paint.tex = paint.tex || makeTexture({});
      box.append(textureEditor(paint.tex, { blend: true, opacity: true }));
    } else if (paint.type === 'solid') {
      box.append(colorRow('色', () => paint.color, (c) => { paint.color = c; }));
    } else if (paint.type === 'linear' || paint.type === 'radial') {
      if (paint.type === 'linear') box.append(num('角度', () => paint.angle ?? 90, (v) => { paint.angle = v; }, { unit: '°' }));
      box.append(gradientEditor(paint));
    } else {
      const dots = paint.type === 'dots';
      box.append(colorRow(dots ? 'ドットの色' : '線の色', () => paint.color, (c) => { paint.color = c; }));
      box.append(colorRow('地の色', () => paint.color2, (c) => { paint.color2 = c; }));
      box.append(num('角度', () => paint.angle ?? 45, (v) => { paint.angle = v; }, { unit: '°' }));
      box.append(num(dots ? 'ドットの直径' : '線の太さ', () => paint.size ?? 10, (v) => { paint.size = v; }, { min: 0.5, step: 0.5, unit: 'px' }));
      box.append(num(dots ? '間隔' : 'すき間', () => paint.gap ?? 10, (v) => { paint.gap = v; }, { min: dots ? 1 : 0, step: 0.5, unit: 'px' }));
    }
  };
  build();
  return box;
}

function setPaintType(p, type) {
  if (type === 'texture') {
    // The paint so far becomes the base the texture is laid over.
    if (p.type !== 'texture') {
      const base = clone(p);
      delete base.tex; delete base.base;
      p.base = base;
      p.tex = p.tex || makeTexture({});
    }
    p.type = 'texture';
    return;
  }
  if (p.type === 'texture') { // leaving the texture: back to the base paint
    const b = p.base;
    delete p.tex; delete p.base;
    if (b) Object.assign(p, b);
  }
  const base = p.color || '#FFFFFFFF';
  p.type = type;
  if ((type === 'linear' || type === 'radial') && !(p.stops && p.stops.length >= 2)) {
    p.stops = [{ pos: 0, color: base }, { pos: 1, color: '#FF000000' }];
    p.angle = p.angle ?? 90;
  }
  if (type === 'stripe' || type === 'dots') {
    p.color = base;
    p.color2 = p.color2 || '#00000000';
    p.angle = p.angle ?? 45;
    p.size = p.size ?? (type === 'dots' ? 10 : 8);
    p.gap = p.gap ?? (type === 'dots' ? 20 : 8);
  }
  if (type === 'solid' && p.stops && p.stops.length) p.color = p.stops[0].color;
}

function gradientEditor(paint) {
  const wrap = el('div', { style: 'display:flex;flex-direction:column;gap:4px' });
  const bar = el('div', { class: 'gradbar' });
  const list = el('div', { style: 'display:flex;flex-direction:column;gap:3px' });
  const refreshBar = () => {
    const stops = [...paint.stops].sort((a, b) => a.pos - b.pos);
    bar.style.background = `linear-gradient(90deg, ${stops.map((s) => `${cssColor(s.color)} ${s.pos * 100}%`).join(',')}),
      repeating-conic-gradient(#666 0 25%, #444 0 50%) 0 0 / 8px 8px`;
  };
  const build = () => {
    list.replaceChildren();
    paint.stops.forEach((s, i) => {
      const pos = el('input', { type: 'number', min: 0, max: 100, value: Math.round(s.pos * 100) });
      pos.oninput = () => { s.pos = clamp(Number(pos.value) / 100, 0, 1); refreshBar(); changed(false); };
      const [picker, alpha, pct] = colorInputs(() => s.color, (c) => { s.color = c; refreshBar(); });
      const del = el('button', { class: 'icon small', text: '×', title: 'この色を削除', disabled: paint.stops.length <= 2,
        onclick: () => { paint.stops.splice(i, 1); build(); changed(false); } });
      list.append(el('div', { class: 'stop' }, el('span', { class: 'dim', text: '位置' }), pos, el('span', { class: 'unit', text: '%' }), picker, alpha, pct, del));
    });
    refreshBar();
  };
  const add = el('button', { class: 'small', text: '＋ 色を追加', onclick: () => {
    const stops = [...paint.stops].sort((a, b) => a.pos - b.pos);
    // Insert in the middle of the widest gap.
    let best = 0;
    for (let i = 1; i < stops.length - 1; i++) if (stops[i + 1].pos - stops[i].pos > stops[best + 1].pos - stops[best].pos) best = i;
    const a = stops[best], b = stops[best + 1] || a;
    paint.stops.push({ pos: (a.pos + b.pos) / 2, color: a.color });
    build();
    changed(false);
  } });
  build();
  wrap.append(bar, list, add);
  return wrap;
}

// ------------------------------------------------------------------ per-character styles
function charStyles(o) {
  const arr = Array.from({ length: o.text.length }, () => ({}));
  for (const s of o.spans || []) {
    for (let i = s.start; i < s.start + s.length && i < arr.length; i++) {
      const st = arr[i];
      for (const k of ['family', 'size', 'weight', 'tracking', 'baselineShift', 'tcy', 'ruby']) if (s[k] !== undefined) st[k] = s[k];
      if (s.paints) st.paints = Object.assign({}, st.paints || {}, clone(s.paints));
    }
  }
  return arr;
}
function styleKey(st) {
  const c = {};
  for (const k of Object.keys(st).sort()) {
    if (st[k] === undefined || st[k] === '' || (k === 'paints' && !Object.keys(st[k]).length)) continue;
    c[k] = st[k];
  }
  return JSON.stringify(c);
}
function writeSpans(o, arr) {
  const spans = [];
  for (let i = 0; i < arr.length;) {
    const key = styleKey(arr[i]);
    let j = i + 1;
    while (j < arr.length && styleKey(arr[j]) === key) j++;
    if (key !== '{}') spans.push(Object.assign({ start: i, length: j - i }, JSON.parse(key)));
    i = j;
  }
  o.spans = spans;
}
function kernArray(o) {
  const arr = new Array(o.text.length).fill(0);
  for (const k of o.kerning || []) if (k.index >= 0 && k.index < arr.length) arr[k.index] += k.value;
  return arr;
}
function writeKern(o, arr) {
  o.kerning = arr.map((v, i) => ({ index: i, value: v })).filter((k) => k.value);
}

// Keeps spans / kerning attached to the right characters when the text is edited.
function setText(o, text) {
  const old = o.text;
  let p = 0;
  while (p < old.length && p < text.length && old[p] === text[p]) p++;
  let s = 0;
  while (s < old.length - p && s < text.length - p && old[old.length - 1 - s] === text[text.length - 1 - s]) s++;
  const removed = old.length - p - s, inserted = text.length - p - s;
  const styles = charStyles(o), kern = kernArray(o);
  // Typed characters take the look of their neighbour, but not its ruby / tate-chu-yoko group.
  const inherit = clone(p > 0 ? styles[p - 1] : (styles[p] || {}));
  delete inherit.ruby;
  delete inherit.tcy;
  styles.splice(p, removed, ...Array.from({ length: inserted }, () => clone(inherit)));
  kern.splice(p, removed, ...new Array(inserted).fill(0));
  o.text = text;
  writeSpans(o, styles);
  writeKern(o, kern);
}

// ------------------------------------------------------------------ properties panel
function buildProps() {
  const props = $('#props');
  const scroll = props.scrollTop;
  props.replaceChildren();
  if (!state.doc) return;
  props.append(objectsSection());
  const o = obj();
  if (isShape(o)) {
    props.append(shapeSection(o), transformSection(o), warpSection(o), stackSection(o), lightSection(o), animSection(o), maskSection(o));
  } else if (o) {
    props.append(textSection(o), transformSection(o), fontSection(o), warpSection(o), rangeSection(o), stackSection(o), lightSection(o), animSection(o), maskSection(o));
  } else {
    props.append(section('ヒント', 'hint', el('div', { class: 'hint', text: 'テキストをクリックして選択します。T ツールでクリックした位置にテキストを追加できます。' })));
  }
  props.append(clipSection());
  props.append(section('情報', 'info',
    el('div', { class: 'hint', text: `描画: ${state.renderer}` }),
    el('div', { class: 'hint', text: `フレーム: ${state.doc.frame.width}×${state.doc.frame.height}　${(state.doc.frame.vidScale / state.doc.frame.vidSampleSize).toFixed(3)} fps` }),
    el('div', { class: 'hint', style: 'user-select:text;word-break:break-all', text: state.file })));
  props.scrollTop = scroll;
  updateStatus();
}

// ---- geometric shapes (a text object whose outline is a figure; see ShapeSpec in tdp_doc.h)
const SHAPE_LABELS = { rect: '四角形', ellipse: '楕円', polygon: '多角形', star: '星', line: '線', arrow: '矢印', bubble: '吹き出し' };
const isShape = (o) => !!o && !!o.shape && !!o.shape.type && o.shape.type !== 'none';
const objLabel = (o) => (isShape(o) ? `図形：${SHAPE_LABELS[o.shape.type] || ''}` : (o.text.split('\n')[0] || o.name));

function makeShapeObject(kind) {
  const d = state.doc.frame;
  const sizes = { rect: [Math.round(d.width * 0.3), Math.round(d.height * 0.16)], ellipse: [Math.round(d.width * 0.2), Math.round(d.height * 0.2)],
    polygon: [220, 200], star: [240, 240], line: [Math.round(d.width * 0.3), 24], arrow: [Math.round(d.width * 0.2), 150], bubble: [Math.round(d.width * 0.3), Math.round(d.height * 0.16)] };
  const o = makeTextObject('●', d.width / 2, d.height / 2);
  o.name = SHAPE_LABELS[kind];
  const fill = makeLayer('fill');
  fill.paint = { type: 'solid', color: '#FF2D8CEB' };
  const stroke = makeLayer('stroke');
  stroke.width = 6;
  stroke.paint = { type: 'solid', color: '#FFFFFFFF' };
  o.stack = [fill, stroke, makeLayer('shadow')];
  o.shape = { type: kind, width: sizes[kind][0], height: sizes[kind][1], radius: kind === 'rect' ? 24 : kind === 'bubble' ? 40 : 0,
    sides: kind === 'polygon' ? 6 : 5, inner: 45, shaft: 40, head: 35, tailX: -25, tailY: 70 };
  return o;
}

function shapeSection(o) {
  const s = o.shape;
  const parts = [select('図形', Object.entries(SHAPE_LABELS), () => s.type, (v) => { s.type = v; o.name = SHAPE_LABELS[v]; }, true),
    num('幅', () => s.width, (v) => { s.width = v; }, { unit: 'px', min: 1 }),
    num('高さ', () => s.height, (v) => { s.height = v; }, { unit: 'px', min: 1 })];
  if (s.type === 'rect' || s.type === 'bubble') parts.push(num('角丸', () => s.radius, (v) => { s.radius = v; }, { unit: 'px', min: 0 }));
  if (s.type === 'polygon') parts.push(num('角の数', () => s.sides, (v) => { s.sides = Math.round(v); }, { min: 3, max: 24 }));
  if (s.type === 'star') {
    parts.push(num('とがりの数', () => s.sides, (v) => { s.sides = Math.round(v); }, { min: 3, max: 24 }));
    parts.push(num('くぼみの深さ', () => s.inner, (v) => { s.inner = v; }, { unit: '%（小さいほど鋭い）', min: 5, max: 95 }));
  }
  if (s.type === 'arrow') {
    parts.push(num('軸の太さ', () => s.shaft, (v) => { s.shaft = v; }, { unit: '%（高さに対して）', min: 5, max: 100 }));
    parts.push(num('矢じりの長さ', () => s.head, (v) => { s.head = v; }, { unit: '%（幅に対して）', min: 5, max: 95 }));
  }
  if (s.type === 'bubble') {
    parts.push(num('しっぽ X', () => s.tailX, (v) => { s.tailX = v; }, { unit: '%（中心から。幅に対して）' }));
    parts.push(num('しっぽ Y', () => s.tailY, (v) => { s.tailY = v; }, { unit: '%（中心から。高さに対して）' }));
  }
  parts.push(el('div', { class: 'hint', text: '四隅の白い四角をドラッグすると大きさを変えられます（Shift で縦横比を保つ）。塗り・フチ・影・3D・テクスチャ・ぼかしは「装飾スタック」、曲げは「ワープ」、アニメーションやマスクは文字と同じに使えます。「回転」と位置は「変形」で設定します。' }));
  return section('図形', 'shape', ...parts);
}

function objectsSection() {
  const list = el('div', { class: 'list' });
  // Top-most object first, like a layer panel.
  for (let i = state.doc.objects.length - 1; i >= 0; i--) {
    const o = state.doc.objects[i];
    const eye = el('span', { class: 'eye' + (o.visible !== false ? ' on' : ''), text: o.visible !== false ? '●' : '○', title: '表示 / 非表示',
      onclick: (e) => { e.stopPropagation(); o.visible = o.visible === false; changed(true); } });
    const item = el('div', { class: 'item' + (i === state.sel ? ' selected' : ''), onclick: () => { selectObject(i); buildProps(); draw(); } },
      eye, el('span', { class: 'name', text: objLabel(o) }));
    list.append(item);
  }
  const up = el('button', { class: 'small', text: '前面へ', onclick: () => moveObject(1) });
  const down = el('button', { class: 'small', text: '背面へ', onclick: () => moveObject(-1) });
  return section('オブジェクト', 'objects', list, el('div', { class: 'stackTools' }, up, down));
}
function moveObject(d) {
  const i = state.sel, j = i + d;
  if (i < 0 || j < 0 || j >= state.doc.objects.length) return;
  const arr = state.doc.objects;
  [arr[i], arr[j]] = [arr[j], arr[i]];
  state.sel = j;
  changed(true);
}

function textSection(o) {
  const ta = el('textarea', { id: 'textInput', rows: 3 });
  ta.value = o.text;
  ta.addEventListener('input', () => { setText(o, ta.value); changed(false); refreshObjectList(); });
  const remember = () => { state.textSel = [ta.selectionStart, ta.selectionEnd]; refreshRange(); };
  ['select', 'keyup', 'mouseup', 'focus'].forEach((ev) => ta.addEventListener(ev, remember));
  return section('テキスト', 'text', ta,
    el('div', { class: 'hint', text: '文字を選択すると「文字ごとの設定」でその範囲だけ変更できます。' }));
}
function refreshObjectList() {
  const items = document.querySelectorAll('#props .section:first-child .item .name');
  const objs = [...state.doc.objects].reverse();
  items.forEach((n, k) => { if (objs[k]) n.textContent = objLabel(objs[k]); });
}

function transformSection(o) {
  if (isShape(o)) {
    return section('変形', 'transform',
      num('X', () => o.x, (v) => { o.x = v; }, { unit: 'px', key: 'x' }),
      num('Y', () => o.y, (v) => { o.y = v; }, { unit: 'px', key: 'y' }),
      num('回転', () => o.rotation, (v) => { o.rotation = v; }, { unit: '°', step: 0.5 }),
      num('不透明度', () => o.opacity, (v) => { o.opacity = v; }, { unit: '%', min: 0, max: 100 }));
  }
  return section('変形', 'transform',
    num('X', () => o.x, (v) => { o.x = v; }, { unit: 'px', key: 'x' }),
    num('Y', () => o.y, (v) => { o.y = v; }, { unit: 'px', key: 'y' }),
    num('回転', () => o.rotation, (v) => { o.rotation = v; }, { unit: '°', step: 0.5 }),
    num('幅', () => o.scaleX, (v) => { o.scaleX = v; }, { unit: '%', min: 1 }),
    num('高さ', () => o.scaleY, (v) => { o.scaleY = v; }, { unit: '%', min: 1 }),
    num('不透明度', () => o.opacity, (v) => { o.opacity = v; }, { unit: '%', min: 0, max: 100 }),
    seg('組み方向', [[false, '横書き'], [true, '縦書き']], () => !!o.vertical, (v) => { o.vertical = v; changed(true); }),
    // Vertical: align runs along the column (left = top), valign picks the side (top = right edge).
    o.vertical
      ? seg('行揃え', [['left', '上'], ['center', '中央'], ['right', '下']], () => o.align, (v) => { o.align = v; })
      : seg('行揃え', [['left', '左'], ['center', '中央'], ['right', '右']], () => o.align, (v) => { o.align = v; }),
    o.vertical
      ? seg('基準（横）', [['top', '右'], ['middle', '中'], ['bottom', '左']], () => o.valign, (v) => { o.valign = v; })
      : seg('基準（縦）', [['top', '上'], ['middle', '中'], ['bottom', '下']], () => o.valign, (v) => { o.valign = v; }),
    ...(o.vertical ? [
      select('縦中横（自動）', [[0, 'しない'], [1, '半角数字 1 桁'], [2, '半角数字 2 桁まで'], [3, '半角数字 3 桁まで'], [4, '半角数字 4 桁まで']],
        () => o.tcyDigits ?? 2, (v) => { o.tcyDigits = Number(v); }),
      tcyCharsInput(o),
      el('div', { class: 'hint', text: '数字の前後に付いた記号も一緒に横に並べます（例: 「()」で (15)、「()[]%」で [3] や 50% も）。「!!」「!?」も自動で横に並びます。ほかの範囲は「文字ごとの設定」で指定できます。' }),
    ] : []));
}
// Symbols joined to an automatic tate-chu-yoko digit group, with one-click presets.
function tcyCharsInput(o) {
  const input = el('input', { type: 'text', value: o.tcyChars ?? '()', placeholder: '（なし）', style: 'width:90px' });
  input.addEventListener('input', () => { o.tcyChars = input.value; changed(false); });
  const preset = (chars) => el('button', { class: 'small', text: chars, onclick: () => {
    for (const c of chars) if (!input.value.includes(c)) input.value += c;
    o.tcyChars = input.value;
    changed(false);
  } });
  return row('一緒に並べる記号', input, preset('()'), preset('[]'), preset('%'), preset('+-'));
}
function syncTransformInputs() {
  const o = obj();
  if (!o) return;
  const x = document.querySelector('input[data-key="x"]'), y = document.querySelector('input[data-key="y"]');
  if (x) x.value = Math.round(o.x);
  if (y) y.value = Math.round(o.y);
  updateStatus();
}

// ------------------------------------------------------------------ font picker
// Fonts come from the host: everything Windows has installed (C:\Windows\Fonts + per-user fonts).
const findFont = (name) => state.fonts.find((f) => f.name === name || f.en === name);
const fontLabel = (name) => { const f = findFont(name); return f ? f.name : name; };
const recentFonts = (() => { try { return JSON.parse(localStorage.getItem('recentFonts') || '[]'); } catch { return []; } })();
function rememberFont(name) {
  const i = recentFonts.indexOf(name);
  if (i >= 0) recentFonts.splice(i, 1);
  recentFonts.unshift(name);
  recentFonts.length = Math.min(recentFonts.length, 8);
  try { localStorage.setItem('recentFonts', JSON.stringify(recentFonts)); } catch { /* ignore */ }
}

// get() -> current family ('' = inherit), set(name or '') ; inheritLabel shown when empty.
function fontInput(get, set, inheritLabel) {
  const btn = el('button', { class: 'fontButton', title: 'フォントを選ぶ' });
  const refresh = () => {
    const v = get();
    btn.textContent = v ? fontLabel(v) : `（${fontLabel(inheritLabel)}）`;
    btn.style.fontFamily = v ? `"${v}", "Yu Gothic UI"` : '';
    if (v && !findFont(v)) btn.classList.add('missing'); else btn.classList.remove('missing');
  };
  refresh();
  btn.onclick = () => openFontPicker(btn, get(), (name) => {
    set(name);
    if (name) rememberFont(name);
    refresh();
    changed(true);
  }, inheritLabel != null);
  return btn;
}

let fontPop = null;
function closeFontPicker() { if (fontPop) { fontPop.remove(); fontPop = null; } }

function openFontPicker(anchor, current, onPick, allowInherit) {
  closeFontPicker();
  const search = el('input', { type: 'text', placeholder: 'フォント名で検索（日本語名・英語名）' });
  const jaOnly = el('input', { type: 'checkbox' });
  try { jaOnly.checked = localStorage.getItem('fontJaOnly') === '1'; } catch { /* ignore */ }
  const list = el('div', { class: 'fontList' });
  const count = el('span', { class: 'dim' });
  fontPop = el('div', { class: 'fontPop' },
    el('div', { class: 'fontPopHead' }, search,
      el('label', { class: 'toggle' }, jaOnly, ' 日本語フォントのみ'), count),
    list,
    el('div', { class: 'hint', style: 'padding:4px 8px', text: 'Windows にインストールされているフォント（C:\\Windows\\Fonts とユーザー単位でインストールしたフォント）を表示しています。' }));
  document.body.append(fontPop);

  const r = anchor.getBoundingClientRect();
  const w = 420, h = Math.min(520, window.innerHeight - 40);
  fontPop.style.width = w + 'px';
  fontPop.style.height = h + 'px';
  fontPop.style.left = clamp(r.right - w, 8, window.innerWidth - w - 8) + 'px';
  fontPop.style.top = clamp(r.bottom + 4, 8, window.innerHeight - h - 8) + 'px';

  // Apply each font only when its row scrolls into view (hundreds of fonts).
  const io = new IntersectionObserver((entries) => {
    for (const en of entries) if (en.isIntersecting) {
      const s = en.target.querySelector('.sample');
      s.style.fontFamily = `"${en.target.dataset.family}", "Yu Gothic UI"`;
      io.unobserve(en.target);
    }
  }, { root: list });

  let rows = [], active = -1;
  const setActive = (i) => {
    if (rows[active]) rows[active].classList.remove('active');
    active = clamp(i, 0, rows.length - 1);
    if (rows[active]) { rows[active].classList.add('active'); rows[active].scrollIntoView({ block: 'nearest' }); }
  };
  const pick = (name) => { closeFontPicker(); onPick(name); };
  const makeRow = (f) => {
    const sample = el('span', { class: 'sample', text: f.ja ? 'あア亜 Abc' : 'Abc 123' });
    const row = el('div', { class: 'fontRow' + (f.name === current || f.en === current ? ' current' : ''), 'data-family': f.en || f.name,
      onclick: () => pick(f.en || f.name) },
      el('span', { class: 'fname', text: f.name, title: f.en ? `${f.name} / ${f.en}` : f.name }), sample);
    io.observe(row);
    return row;
  };
  const build = () => {
    const q = search.value.trim().toLowerCase();
    const match = (f) => (!jaOnly.checked || f.ja) && (!q || f.name.toLowerCase().includes(q) || (f.en || '').toLowerCase().includes(q));
    list.replaceChildren();
    rows = [];
    if (allowInherit && !q) {
      const row = el('div', { class: 'fontRow' + (!current ? ' current' : ''), onclick: () => pick('') }, el('span', { class: 'fname', text: '（全体と同じ）' }));
      list.append(row); rows.push(row);
    }
    const recent = q ? [] : recentFonts.map(findFont).filter((f) => f && match(f));
    if (recent.length) {
      list.append(el('div', { class: 'fontGroup', text: '最近使ったフォント' }));
      for (const f of recent) { const row = makeRow(f); list.append(row); rows.push(row); }
      list.append(el('div', { class: 'fontGroup', text: 'すべてのフォント' }));
    }
    let n = 0;
    for (const f of state.fonts) {
      if (!match(f)) continue;
      const row = makeRow(f); list.append(row); rows.push(row); n++;
    }
    count.textContent = `${n} 件`;
    active = -1;
    const cur = rows.findIndex((x) => x.classList.contains('current'));
    if (cur >= 0 && !q) setActive(cur);
  };
  search.addEventListener('input', build);
  jaOnly.addEventListener('change', () => { try { localStorage.setItem('fontJaOnly', jaOnly.checked ? '1' : '0'); } catch { /* ignore */ } build(); });
  search.addEventListener('keydown', (e) => {
    if (e.key === 'ArrowDown') { setActive(active + 1); e.preventDefault(); }
    else if (e.key === 'ArrowUp') { setActive(active - 1); e.preventDefault(); }
    else if (e.key === 'Enter' && rows[active]) rows[active].click();
    else if (e.key === 'Escape') closeFontPicker();
  });
  build();
  search.focus();
}
document.addEventListener('pointerdown', (e) => {
  if (fontPop && !fontPop.contains(e.target) && !e.target.closest('.fontButton')) closeFontPicker();
}, true);

// Weight list with the weights the family really has marked; others are synthesized by Windows.
function weightOptions(family) {
  const f = findFont(family);
  const has = new Set(f ? f.weights : []);
  return WEIGHTS.map(([v, t]) => [v, has.has(v) ? t + '　●' : t]);
}

function fontSection(o) {
  return section('フォント', 'font',
    row('フォント', fontInput(() => o.family, (v) => { o.family = v || 'Yu Gothic UI'; })),
    num('サイズ', () => o.size, (v) => { o.size = v; }, { unit: 'px', min: 1 }),
    select('太さ', weightOptions(o.family), () => o.weight, (v) => { o.weight = Number(v); }),
    el('div', { class: 'hint', text: '● はフォントに実在する太さ。それ以外は近い太さで描かれます。' }),
    check('斜体', () => o.italic, (v) => { o.italic = v; }),
    num('字間', () => o.tracking, (v) => { o.tracking = v; }, { unit: 'px', step: 0.5 }),
    seg('字詰め', [['metrics', 'メトリクス'], ['palt', '詰め（palt）'], ['none', 'なし']], () => o.kernMode || 'metrics', (v) => { o.kernMode = v; }),
    num('オプティカル調整', () => o.opticalKern || 0, (v) => { o.opticalKern = v; }, { unit: '%（0 = 使わない）', min: 0, max: 200 }),
    row('', el('button', { class: 'small', text: 'カーニングツール（K）', title: 'プレビュー上で文字の間を選んで ← → で調整', onclick: () => setTool('kern') }),
      el('button', { class: 'small', text: '手動カーニングを消す', disabled: !(o.kerning || []).length, onclick: () => { o.kerning = []; state.kernSel = null; changed(true); } })),
    el('div', { class: 'hint', text: 'メトリクス = フォント内蔵のカーニング。詰め（palt）= 和文をプロポーショナルに詰める（対応フォントのみ。游ゴシック・Noto Sans JP など）。オプティカル = 文字の形から隙間を自動で均す（文字種ごと）。手動のカーニングはその上に足されます。手動は「カーニングツール」で文字の間をクリックして ← → か、テキスト欄で Alt + ← →。' }),
    num('行の高さ', () => o.lineHeight, (v) => { o.lineHeight = v; }, { unit: 'px（0 = 自動）', min: 0 }),
    check('グラデーション・模様を1行ごとに', () => o.paintPerLine ?? true, (v) => { o.paintPerLine = v; }),
    num('ルビの大きさ', () => o.rubyScale ?? 50, (v) => { o.rubyScale = v; }, { unit: '%', min: 10, max: 200 }),
    num('ルビの間隔', () => o.rubyGap ?? 0, (v) => { o.rubyGap = v; }, { unit: 'px', step: 0.5 }),
    el('div', { class: 'hint', text: 'ルビはテキスト欄で文字を選び、「文字ごとの設定」の「ルビ」に入力します。' }));
}

// Mirrors Warp / Mask in tdp_doc.h.
const WARP_TYPES = [['none', 'なし'], ['arc', '円弧（円に沿って曲げる）'], ['arch', 'アーチ（山なり）'], ['bulge', '膨らみ'], ['flag', '旗'], ['wave', '波'],
  ['rise', '上昇（右が高くなる）'], ['fisheye', '魚眼'], ['squeeze', '絞り']];
const MASK_DEFAULTS = { type: 'none', angle: 0, softness: 30, invert: false, pos: 100, wipeIn: 0, wipeInDelay: 0, wipeOut: 0, wipeOutDelay: 0 };

function warpSection(o) {
  const w = o.warp = Object.assign({ type: 'none', amount: 50, vertical: false }, o.warp);
  const parts = [select('形', WARP_TYPES, () => w.type, (v) => { w.type = v; }, true)];
  if (w.type !== 'none') {
    parts.push(num('量', () => w.amount, (v) => { w.amount = v; }, { unit: '%（負で逆向き）', min: -100, max: 100 }));
    parts.push(seg('向き', [[false, '横方向（上下に曲がる）'], [true, '縦方向（左右に曲がる）']], () => !!w.vertical, (v) => { w.vertical = v; }));
    parts.push(el('div', { class: 'hint', text: '文字の並び全体（複数行も 1 つのまとまり）を曲げます。フチ・影・3D・アニメーションは曲げた形に対してかかります。文字ごとの 3D 回転などのアニメは、曲げた後の位置で動きます。' }));
  }
  return section('テキストの変形（ワープ）', 'warp', ...parts);
}

function maskSection(o) {
  const m = o.mask = Object.assign({}, MASK_DEFAULTS, o.mask);
  const parts = [seg('種類', [['none', 'なし'], ['linear', '線形'], ['radial', '円形']], () => m.type, (v) => { m.type = v; changed(true); })];
  if (m.type !== 'none') {
    if (m.type === 'linear') {
      parts.push(seg('進む向き', [[0, '→ 右へ'], [90, '↓ 下へ'], [180, '← 左へ'], [270, '↑ 上へ']], () => m.angle, (v) => { m.angle = v; changed(true); }));
      parts.push(num('角度', () => m.angle, (v) => { m.angle = v; }, { unit: '°（0 = 右へ, 90 = 下へ）' }));
    }
    parts.push(num('ぼかし幅', () => m.softness, (v) => { m.softness = v; }, { unit: '%（境目のやわらかさ）', min: 0, max: 200 }));
    parts.push(num('位置', () => m.pos, (v) => { m.pos = v; }, { unit: '%（0 = 全部隠す, 100 = 全部見せる）' }));
    parts.push(check('反対側を見せる', () => m.invert, (v) => { m.invert = v; }));
    parts.push(el('div', { class: 'animTitle', text: 'ワイプ' }));
    parts.push(num('登場の長さ', () => m.wipeIn, (v) => { m.wipeIn = v; }, { unit: 'フレーム（0 = なし）', min: 0 }));
    if (m.wipeIn > 0) parts.push(num('登場の遅れ', () => m.wipeInDelay, (v) => { m.wipeInDelay = v; }, { unit: 'フレーム', min: 0 }));
    parts.push(num('退場の長さ', () => m.wipeOut, (v) => { m.wipeOut = v; }, { unit: 'フレーム（0 = なし。クリップの最後で終わる）', min: 0 }));
    if (m.wipeOut > 0) parts.push(num('退場の余白', () => m.wipeOutDelay, (v) => { m.wipeOutDelay = v; }, { unit: 'フレーム', min: 0 }));
    parts.push(el('div', { class: 'hint', text: '全部の層（影や 3D も）をまとめて、境目より先を透明にします。「位置」を下げたままにすると、下半分だけ薄くなどの固定のフェードに、ワイプを使うと、境目が走って現れる・消える動きになります（登場時は 0 → 位置、退場は位置 → 0）。円形は中心から広がる（絞る）動きです。' }));
  }
  return section('フェードマスク（ワイプ）', 'mask', ...parts);
}

// "文字ごとの設定": applies to the range selected in the text box.
function rangeSection(o) {
  const body = el('div', { id: 'rangeBody', style: 'display:flex;flex-direction:column;gap:5px' });
  const s = section('文字ごとの設定', 'range', body);
  setTimeout(refreshRange);
  return s;
}

function refreshRange() {
  const body = $('#rangeBody');
  const o = obj();
  if (!body || !o) return;
  body.replaceChildren();
  let [a, b] = state.textSel;
  a = clamp(a, 0, o.text.length); b = clamp(b, 0, o.text.length);
  if (a === b) {
    body.append(el('div', { class: 'hint', text: 'テキスト欄で文字を選択してください。' }));
    // Kerning between the character before the caret and the one after it.
    if (a > 0 && a < o.text.length) {
      const kern = kernArray(o);
      body.append(num(`カーニング「${o.text[a - 1]}${o.text[a]}」`, () => kern[a - 1], (v) => { kern[a - 1] = v; writeKern(o, kern); }, { unit: 'px', step: 0.5 }));
    }
    return;
  }
  const styles = charStyles(o);
  const first = styles[a];
  const update = (fn) => { const arr = charStyles(o); for (let i = a; i < b; i++) fn(arr[i]); writeSpans(o, arr); };
  body.append(el('div', { class: 'hint', text: `選択: 「${o.text.slice(a, b).replace(/\n/g, '⏎')}」（${b - a} 文字）` }));
  body.append(row('フォント', fontInput(() => first.family || '', (v) => update((st) => { if (v) st.family = v; else delete st.family; }), o.family)));
  const optNum = (label, key, unit, step) => {
    const input = el('input', { type: 'number', step: step || 1, placeholder: '—', value: first[key] ?? '' });
    input.addEventListener('input', () => {
      update((st) => { if (input.value === '') delete st[key]; else st[key] = Number(input.value); });
      changed(false);
    });
    return row(label, input, el('span', { class: 'unit', text: unit }));
  };
  body.append(optNum('サイズ', 'size', 'px'));
  body.append(select('太さ', [['', '（全体と同じ）'], ...WEIGHTS], () => first.weight ?? '', (v) => update((st) => { if (v) st.weight = Number(v); else delete st.weight; })));
  body.append(optNum('字間（追加）', 'tracking', 'px', 0.5));
  body.append(optNum('ベースライン', 'baselineShift', 'px（+ で上）', 0.5));
  {
    // The whole selection gets one ruby, centred over it.
    const r = el('input', { type: 'text', placeholder: '（なし）', value: first.ruby || '' });
    r.addEventListener('input', () => {
      update((st) => { if (r.value) st.ruby = r.value; else delete st.ruby; });
      changed(false);
    });
    body.append(row('ルビ', r));
  }
  if (o.vertical) {
    const t = el('input', { type: 'checkbox' });
    t.checked = !!first.tcy;
    t.onchange = () => { update((st) => { if (t.checked) st.tcy = true; else delete st.tcy; }); changed(false); };
    body.append(row('縦中横', t));
  }

  // Per-layer paint overrides (fill / stroke only).
  for (const l of o.stack.filter((x) => x.type === 'fill' || x.type === 'stroke')) {
    const has = !!(first.paints && first.paints[l.id]);
    const c = el('input', { type: 'checkbox' });
    c.checked = has;
    const wrap = el('div');
    c.onchange = () => {
      update((st) => {
        st.paints = st.paints || {};
        if (c.checked) st.paints[l.id] = clone(l.paint); else delete st.paints[l.id];
      });
      changed(false);
      refreshRange();
    };
    body.append(row(`${l.name} の色を変える`, c));
    if (has) {
      // Edit a copy, then write it to every character of the range.
      const p = clone(first.paints[l.id]);
      const ed = paintEditor(p, null, { noTexture: true }); // a texture is laid over the whole layer, not per character
      ed.addEventListener('input', () => setTimeout(() => { update((st) => { st.paints = st.paints || {}; st.paints[l.id] = clone(p); }); changed(false); }));
      ed.addEventListener('change', () => setTimeout(() => { update((st) => { st.paints = st.paints || {}; st.paints[l.id] = clone(p); }); changed(false); }));
      wrap.append(ed);
      body.append(wrap);
    }
  }
  body.append(el('button', { class: 'small', text: 'この範囲の設定をクリア', onclick: () => { update((st) => { for (const k of Object.keys(st)) delete st[k]; }); changed(false); refreshRange(); } }));
}

function stackSection(o) {
  const list = el('div', { class: 'list' });
  let dragIndex = -1;
  o.stack.forEach((l, i) => {
    const swatchColor = l.type === 'extrude' ? l.sideColor : l.type === 'texture' ? '#FFB4B4B4' : l.paint ? paintSwatch(l.paint) : l.color;
    const item = el('div', { class: 'item' + (l.id === state.layerSel ? ' selected' : ''), draggable: 'true',
      onclick: () => { state.layerSel = l.id; buildProps(); } },
      el('span', { class: 'eye' + (l.enabled ? ' on' : ''), text: l.enabled ? '●' : '○', title: '有効 / 無効',
        onclick: (e) => { e.stopPropagation(); l.enabled = !l.enabled; changed(true); } }),
      el('span', { class: 'swatch', style: `background:${cssColor(swatchColor)}` }),
      el('span', { class: 'name', text: l.name }),
      el('span', { class: 'kind', text: LAYER_LABELS[l.type] }));
    item.addEventListener('dragstart', (e) => { dragIndex = i; item.classList.add('dragging'); e.dataTransfer.effectAllowed = 'move'; });
    item.addEventListener('dragend', () => item.classList.remove('dragging'));
    item.addEventListener('dragover', (e) => {
      e.preventDefault();
      const after = e.offsetY > item.offsetHeight / 2;
      item.classList.toggle('drop-before', !after);
      item.classList.toggle('drop-after', after);
    });
    item.addEventListener('dragleave', () => item.classList.remove('drop-before', 'drop-after'));
    item.addEventListener('drop', (e) => {
      e.preventDefault();
      const after = e.offsetY > item.offsetHeight / 2;
      if (dragIndex < 0) return;
      const [moved] = o.stack.splice(dragIndex, 1);
      let to = i + (after ? 1 : 0);
      if (dragIndex < to) to--;
      o.stack.splice(to, 0, moved);
      changed(true);
    });
    list.append(item);
  });

  const addSel = el('select', { onchange: () => {
    if (!addSel.value) return;
    const l = makeLayer(addSel.value);
    let idx = Math.max(0, o.stack.findIndex((x) => x.id === state.layerSel));
    // 3D extrudes and a texture overlay covers the layers above it: by default they go right under the last fill / stroke.
    if (l.type === 'extrude' || l.type === 'texture') idx = o.stack.reduce((last, x, i) => (x.type === 'fill' || x.type === 'stroke' ? i : last), -1);
    o.stack.splice(o.stack.length ? idx + 1 : 0, 0, l);
    state.layerSel = l.id;
    changed(true);
  } }, el('option', { value: '', text: '＋ 追加…' }), Object.entries(LAYER_LABELS).map(([v, t]) => el('option', { value: v, text: t })));
  const idx = o.stack.findIndex((x) => x.id === state.layerSel);
  const tools = el('div', { class: 'stackTools' }, addSel,
    el('button', { class: 'small', text: '↑', title: '手前へ', disabled: idx <= 0, onclick: () => moveLayer(o, idx, -1) }),
    el('button', { class: 'small', text: '↓', title: '奥へ', disabled: idx < 0 || idx >= o.stack.length - 1, onclick: () => moveLayer(o, idx, 1) }),
    el('button', { class: 'small', text: '複製', disabled: idx < 0, onclick: () => {
      const c = clone(o.stack[idx]); c.id = newId('L'); c.name += ' のコピー';
      o.stack.splice(idx + 1, 0, c); state.layerSel = c.id; changed(true);
    } }),
    el('button', { class: 'small', text: '削除', disabled: idx < 0, onclick: () => {
      o.stack.splice(idx, 1);
      state.layerSel = o.stack[Math.min(idx, o.stack.length - 1)]?.id ?? null;
      changed(true);
    } }));

  const parts = [el('div', { class: 'hint', text: '上ほど手前に描かれます。ドラッグで並べ替え。シャドウ系は、それより上にある層をまとめた形の影になります。3D は、それより上にある塗り・フチの形を押し出し、その見た目を正面に貼ります（3D は 1 つだけ有効）。テクスチャ（重ね）は、それより上にある層に画像・動画を重ねます。' }), list, tools];
  const l = layerOf(o);
  if (l) parts.push(layerEditor(o, l));
  return section('装飾スタック', 'stack', ...parts);
}
function moveLayer(o, i, d) {
  const j = i + d;
  if (i < 0 || j < 0 || j >= o.stack.length) return;
  [o.stack[i], o.stack[j]] = [o.stack[j], o.stack[i]];
  changed(true);
}

// Blur of the finished layer: a blurred stroke is an outline glow, a blurred white fill an inner glow.
const softenRow = (l) => num('ぼかし', () => l.soften || 0, (v) => { l.soften = v; }, { unit: 'px（0 = なし。ぼかしたフチ＝輪郭グロー）', min: 0, step: 0.5 });

function layerEditor(o, l) {
  const nameInput = el('input', { type: 'text', value: l.name });
  nameInput.addEventListener('change', () => { l.name = nameInput.value || LAYER_LABELS[l.type]; changed(true); });
  const parts = [row('名前', nameInput), num('不透明度', () => l.opacity, (v) => { l.opacity = v; }, { unit: '%', min: 0, max: 100 })];
  switch (l.type) {
    case 'texture':
      parts.push(check('文字の形に制限', () => l.clip !== false, (v) => { l.clip = v; }));
      l.tex = l.tex || makeTexture({ blend: 'screen' });
      parts.push(textureEditor(l.tex, { blend: true, opacity: true }));
      parts.push(el('div', { class: 'hint', text: 'それより上にある層（塗り・フチなど）の上に、画像・動画を重ねます。「文字の形に制限」をオフにすると、文字の外にも描きます（粒子や光の素材向け）。透過動画（ProRes 4444 など）はアルファが使えます。' }));
      break;
    case 'fill':
      parts.push(paintEditor(l.paint));
      parts.push(softenRow(l));
      parts.push(surfaceEditor(o, l));
      break;
    case 'stroke':
      parts.push(num('太さ', () => l.width, (v) => { l.width = v; }, { unit: 'px', min: 0, step: 0.5 }));
      parts.push(seg('位置', [['outer', '外側'], ['center', '中央'], ['inner', '内側']], () => l.position, (v) => { l.position = v; }));
      parts.push(seg('角', [['round', 'ラウンド'], ['miter', 'マイター'], ['bevel', 'ベベル']], () => l.join, (v) => { l.join = v; }));
      parts.push(num('マイター上限', () => l.miterLimit, (v) => { l.miterLimit = v; }, { min: 1, step: 0.5 }));
      parts.push(paintEditor(l.paint));
      parts.push(softenRow(l));
      parts.push(surfaceEditor(o, l));
      break;
    case 'shadow':
    case 'innerShadow':
      parts.push(colorRow('色', () => l.color, (c) => { l.color = c; }));
      parts.push(num('角度', () => l.angle, (v) => { l.angle = v; }, { unit: '°（0 = 右, 90 = 下）' }));
      parts.push(num('距離', () => l.distance, (v) => { l.distance = v; }, { unit: 'px', min: 0, step: 0.5 }));
      parts.push(num('ぼかし', () => l.blur, (v) => { l.blur = v; }, { unit: 'px', min: 0, step: 0.5 }));
      parts.push(num(l.type === 'shadow' ? '広がり' : 'チョーク', () => l.spread, (v) => { l.spread = v; }, { unit: 'px', min: 0, max: 49, step: 0.5 }));
      break;
    case 'longShadow':
      parts.push(colorRow('色', () => l.paint.color, (c) => { l.paint.color = c; }));
      parts.push(num('角度', () => l.angle, (v) => { l.angle = v; }, { unit: '°（45 = 右下）' }));
      parts.push(num('長さ', () => l.length, (v) => { l.length = v; }, { unit: 'px', min: 0 }));
      break;
    case 'background':
      parts.push(num('余白（左右）', () => l.padX, (v) => { l.padX = v; }, { unit: 'px' }));
      parts.push(num('余白（上下）', () => l.padY, (v) => { l.padY = v; }, { unit: 'px' }));
      parts.push(num('角丸', () => l.radius, (v) => { l.radius = v; }, { unit: 'px', min: 0 }));
      parts.push(paintEditor(l.paint));
      parts.push(softenRow(l));
      break;
    case 'extrude':
      parts.push(el('div', { class: 'animTitle', text: '押し出し' }));
      parts.push(num('深さ', () => l.depth, (v) => { l.depth = v; }, { unit: 'px', min: 0 }));
      parts.push(presetSelect('側面の材質', SIDE_PRESETS, (p) => { Object.assign(l, p[1]); studioFor(o, p[1].metallic); }));
      parts.push(colorRow('側面（手前）', () => l.sideColor, (c) => { l.sideColor = c; }));
      parts.push(colorRow('側面（奥）', () => l.sideColor2, (c) => { l.sideColor2 = c; }));
      parts.push(el('div', { class: 'animTitle', text: 'ベベル' }));
      parts.push(seg('形', [['none', 'なし'], ['flat', '平ら'], ['round', '丸み'], ['cove', 'くぼみ']], () => l.bevel, (v) => { l.bevel = v; }));
      parts.push(num('幅', () => l.bevelWidth, (v) => { l.bevelWidth = v; }, { unit: 'px', min: 0, step: 0.5 }));
      parts.push(num('高さ', () => l.bevelHeight, (v) => { l.bevelHeight = v; }, { unit: 'px', min: 0, step: 0.5 }));
      parts.push(el('div', { class: 'animTitle', text: '向き' }));
      parts.push(num('X 回転（上下に傾ける）', () => l.rotX, (v) => { l.rotX = v; }, { unit: '°' }));
      parts.push(num('Y 回転（左右に振る）', () => l.rotY, (v) => { l.rotY = v; }, { unit: '°' }));
      parts.push(num('パース', () => l.perspective, (v) => { l.perspective = v; }, { min: 0, max: 100 }));
      parts.push(el('div', { class: 'animTitle', text: 'ターンテーブル（ずっと回る）' }));
      parts.push(seg('動き', [['none', 'なし'], ['spin', '回り続ける'], ['sway', 'ゆらゆら揺れる']], () => l.turn || 'none', (v) => { l.turn = v; changed(true); }));
      if (l.turn === 'spin' || l.turn === 'sway') {
        parts.push(seg('軸', [[false, '縦軸（左右に回る）'], [true, '横軸（上下に回る）']], () => !!l.turnX, (v) => { l.turnX = v; }));
        if (l.turn === 'spin') {
          parts.push(num('回る速さ', () => l.turnSpeed ?? 90, (v) => { l.turnSpeed = v; }, { unit: '°/秒（負で逆回り）' }));
          parts.push(num('開始の角度', () => l.turnPhase ?? 0, (v) => { l.turnPhase = v; }, { unit: '°' }));
        } else {
          parts.push(num('振れ幅', () => l.turnAmount ?? 25, (v) => { l.turnAmount = v; }, { unit: '°（上の向きを中心に左右へ）' }));
          parts.push(num('1往復の時間', () => l.turnPeriod ?? 3, (v) => { l.turnPeriod = v; }, { unit: '秒', min: 0.1, step: 0.1 }));
          parts.push(num('位相', () => l.turnPhase ?? 0, (v) => { l.turnPhase = v; }, { unit: '°' }));
        }
        parts.push(el('div', { class: 'hint', text: '上の「向き」に、時間に応じた回転を足します（クリップの頭から）。登場・退場の動きとは別に、クリップの間ずっと続きます。' }));
      }
      parts.push(el('div', { class: 'animTitle', text: '側面の材質' }));
      parts.push(num('金属っぽさ', () => l.metallic, (v) => { l.metallic = v; }, { unit: '%', min: 0, max: 100 }));
      parts.push(num('粗さ', () => l.roughness, (v) => { l.roughness = v; }, { unit: '%（0 = つるつる）', min: 0, max: 100 }));
      parts.push(el('div', { class: 'hint', text: '正面は上の層（塗り・フチ）の見た目そのままです（塗り・フチの「質感」で金属やベベルを付けられます）。ベベルと側面に光と材質がかかります。光は「ライトと映り込み」で設定します。下にある影や座布団は 3D の結果にかかります（座布団は回転しません）。' }));
      break;
  }
  return el('div', { style: 'display:flex;flex-direction:column;gap:5px;margin-top:6px' }, ...parts);
}

// ------------------------------------------------------------------ animation
// Mirrors AnimSpec in tdp_doc.h. The motion itself is evaluated natively (tdp_doc.cpp EvalAnim).
const ANIM_DEFAULTS = { unit: 'char', order: 'forward', ease: 'auto', dir: 'left', duration: 12, stagger: 2, delay: 0, amount: 0, fade: true, seed: 1 };
const ANIM_TYPES_IN = [['none', 'なし'], ['fade', 'フェード'], ['typewriter', 'タイプライター'], ['pop', 'ポップ（拡大して出る）'],
  ['zoom', 'ズーム（大きい所から）'], ['drop', '落下・バウンド'], ['rise', '飛び出し（下から）'], ['slide', 'スライド'], ['spin', '回転'], ['flip3d', '3D 回転'],
  ['flyz', '3D 奥行き（奥から飛んでくる）'], ['hinge', '3D ヒンジ（倒れる・扉が開く）'], ['tumble', '3D 回転（斜めにくるくる）'], ['grow', '3D 押し出しが伸びる']];
const ANIM_TYPES_OUT = [['none', 'なし'], ['fade', 'フェード'], ['typewriter', 'タイプライター（消していく）'], ['pop', 'ポップ（縮んで消える）'],
  ['zoom', 'ズーム（大きくなって消える）'], ['drop', '落下（下へ落ちる）'], ['rise', '沈む（下へ）'], ['slide', 'スライド'], ['spin', '回転'], ['flip3d', '3D 回転'],
  ['flyz', '3D 奥行き（奥へ飛んでいく）'], ['hinge', '3D ヒンジ（倒れる・扉が閉じる）'], ['tumble', '3D 回転（斜めにくるくる）'], ['grow', '3D 押し出しが縮む']];
// [label, unit, type default]
const ANIM_AMOUNT = { pop: ['開始の大きさ', '%', 0], zoom: ['開始の大きさ', '%', 300], drop: ['高さ', 'px', 160], rise: ['深さ', 'px', 80],
  slide: ['距離', 'px', 120], spin: ['回転', '°', 360], flip3d: ['回転', '°', 90], flyz: ['奥行きの距離', 'px', 800], hinge: ['回転', '°', 90],
  tumble: ['回転', '°', 360] };
const ANIM_3D = ['flip3d', 'hinge', 'tumble', 'flyz', 'grow'];
const ANIM_UNITS = [['char', '1文字ずつ'], ['line', '1行ずつ'], ['all', '全体まとめて']];
const ANIM_ORDERS = [['forward', '先頭から'], ['backward', '末尾から'], ['centerOut', '中央から外へ'], ['edgesIn', '両端から中央へ'], ['random', 'ランダム']];
const ANIM_EASES = [['auto', '動きに合わせる'], ['linear', '一定'], ['out', 'だんだん遅く'], ['inOut', 'ゆっくり→速く→ゆっくり'],
  ['back', '行き過ぎて戻る'], ['bounce', '弾む'], ['elastic', 'ばね']];

function animUnitCount(o, a) {
  if (a.unit === 'all') return 1;
  if (a.unit === 'line') return o.text.split('\n').length;
  return [...o.text].filter((c) => !/[\s　]/.test(c)).length;
}
function animSlots(a, count) {
  if (count <= 1) return 1;
  return a.order === 'centerOut' || a.order === 'edgesIn' ? Math.floor((count - 1) / 2) + 1 : count;
}
// Frames from the first unit starting to the last unit finishing (delay included).
function animSpan(o, a) {
  if (!a || a.type === 'none') return 0;
  const d = a.type === 'typewriter' ? 1 : a.duration;
  return a.delay + (animSlots(a, animUnitCount(o, a)) - 1) * a.stagger + d;
}

function animEditor(o, key, out) {
  const a = o[key] = Object.assign({}, ANIM_DEFAULTS, o[key] || { type: 'none' });
  const parts = [select('動き', out ? ANIM_TYPES_OUT : ANIM_TYPES_IN, () => a.type, (v) => { a.type = v; a.amount = 0; }, true)];
  if (a.type !== 'none') {
    parts.push(select('単位', ANIM_UNITS, () => a.unit, (v) => { a.unit = v; }, true));
    if (a.unit !== 'all') {
      parts.push(select('順番', ANIM_ORDERS, () => a.order, (v) => { a.order = v; }, true));
      if (a.order === 'random') parts.push(row('', el('button', { class: 'small', text: '並びを変える', onclick: () => { a.seed = (a.seed % 9973) + 1; changed(false); } })));
      parts.push(num(a.type === 'typewriter' ? '1文字の間隔' : '次の文字まで', () => a.stagger, (v) => { a.stagger = v; }, { unit: 'フレーム', min: 0, step: 0.5 }));
    }
    if (a.type !== 'typewriter') parts.push(num('1つの長さ', () => a.duration, (v) => { a.duration = v; }, { unit: 'フレーム', min: 0, step: 0.5 }));
    parts.push(num(out ? '終わりの余白' : '開始の遅れ', () => a.delay, (v) => { a.delay = v; }, { unit: 'フレーム', min: 0 }));
    const am = ANIM_AMOUNT[a.type];
    if (am) parts.push(num(am[0], () => a.amount || am[2], (v) => { a.amount = v; }, { unit: am[1] }));
    if (a.type === 'slide')
      parts.push(seg(out ? '去る方向' : '来る方向', [['left', '左'], ['right', '右'], ['up', '上'], ['down', '下']], () => a.dir, (v) => { a.dir = v; }));
    if (a.type === 'flip3d' || a.type === 'tumble')
      parts.push(seg('回る向き', [['left', '左から'], ['right', '右から'], ['up', '上から'], ['down', '下から']], () => a.dir, (v) => { a.dir = v; }));
    if (a.type === 'flyz')
      parts.push(seg('来る方向', [['left', '奥から'], ['right', '手前から']], () => a.dir, (v) => { a.dir = v; }));
    if (a.type === 'hinge')
      parts.push(seg('軸にする辺', [['left', '左端'], ['right', '右端'], ['up', '上端'], ['down', '下端（ドミノ）']], () => a.dir, (v) => { a.dir = v; }));
    if (ANIM_3D.includes(a.type) && !o.stack.some((l) => l.type === 'extrude' && l.enabled))
      parts.push(el('div', { class: 'hint', text: a.type === 'grow'
        ? '装飾スタックに 3D 層が無いので、何も起きません。'
        : '装飾スタックに 3D 層が無いので、縮めたり大きさを変えたりする疑似表現になります。' }));
    if (a.type !== 'typewriter' && a.type !== 'fade') {
      parts.push(select('動きのカーブ', ANIM_EASES, () => a.ease, (v) => { a.ease = v; }));
      if (a.type !== 'grow') parts.push(check('一緒にフェード', () => a.fade, (v) => { a.fade = v; }));
    }
    const span = animSpan(o, a);
    const dur = state.doc.frame.durationFrames;
    parts.push(el('div', { class: 'hint', text: out
      ? `${span} フレームかけて消え、クリップの最後（${dur} フレーム目）で消え終わります。`
      : `${span} フレーム目で出そろいます。` }));
    parts.push(row('', el('button', { class: 'small', text: out ? '▶ 退場を再生' : '▶ 登場を再生', onclick: () => {
      startPlay(out ? Math.max(0, dur - span - 10) : 0, out ? dur - 1 : Math.min(dur - 1, span + 15));
    } })));
  }
  return el('div', { class: 'animBox' }, el('div', { class: 'animTitle', text: out ? '退場' : '登場' }), ...parts);
}

function animSection(o) {
  return section('アニメーション', 'anim',
    animEditor(o, 'animIn', false),
    animEditor(o, 'animOut', true),
    el('div', { class: 'hint', text: '退場は、クリップの長さ（下の「クリップ」）の最後に合わせて再生されます。Premiere でクリップの終わりを短く切ると、退場も一緒に切れます。' }));
}

function clipSection() {
  const f = state.doc.frame;
  const fps = f.vidScale / f.vidSampleSize;
  return section('クリップ', 'clip',
    num('長さ', () => f.durationFrames, (v) => { f.durationFrames = Math.max(1, Math.round(v)); updateDuration(); }, { unit: `フレーム（${(f.durationFrames / fps).toFixed(2)} 秒）`, min: 1 }),
    el('div', { class: 'hint', text: '保存すると Premiere の素材の長さも変わります。タイムラインに置いたクリップの長さは自動では伸びません。' }));
}
function updateDuration() {
  const slider = $('#frameSlider');
  slider.max = Math.max(0, state.doc.frame.durationFrames - 1);
  if (state.frame > Number(slider.max)) state.frame = Number(slider.max);
  slider.value = state.frame;
  updateFrameLabel();
}

// A frame where every object has finished coming in (and has not started leaving).
function restFrame() {
  const dur = state.doc.frame.durationFrames;
  let f = 0;
  for (const o of state.doc.objects) f = Math.max(f, animSpan(o, o.animIn));
  return Math.min(f, dur - 1);
}

// ------------------------------------------------------------------ playback
const play = { on: false, t0: 0, from: 0, to: 0, loop: true };
function setFrame(f) {
  state.frame = f;
  $('#frameSlider').value = f;
  updateFrameLabel();
  requestRender();
}
function startPlay(from, to) {
  const dur = state.doc.frame.durationFrames;
  play.from = clamp(from ?? 0, 0, dur - 1);
  play.to = clamp(to ?? dur - 1, play.from, dur - 1);
  play.loop = from == null;
  play.on = true;
  play.t0 = performance.now();
  $('#btnPlay').textContent = '■';
  requestAnimationFrame(tick);
}
function stopPlay() {
  play.on = false;
  $('#btnPlay').textContent = '▶';
}
function tick(now) {
  if (!play.on) return;
  const f0 = state.doc.frame;
  const fps = f0.vidScale / f0.vidSampleSize;
  let f = play.from + Math.floor((now - play.t0) / 1000 * fps);
  if (f > play.to) {
    if (!play.loop) { setFrame(play.to); stopPlay(); return; }
    play.t0 = now;
    f = play.from;
  }
  if (f !== state.frame) setFrame(f);
  requestAnimationFrame(tick);
}
// Whole clip, looping, continuing from the current frame.
function togglePlay() {
  if (play.on) { stopPlay(); return; }
  const f = state.doc.frame;
  const start = state.frame < f.durationFrames - 1 ? state.frame : 0;
  startPlay();
  play.t0 -= start / (f.vidScale / f.vidSampleSize) * 1000;
}
$('#btnPlay').onclick = togglePlay;

// ------------------------------------------------------------------ presets
function defaultPresets() {
  const solid = (c) => ({ type: 'solid', color: c });
  const fill = (paint) => ({ id: 'F', type: 'fill', name: '塗り', enabled: true, opacity: 100, paint });
  const stroke = (id, w, paint, name) => ({ id, type: 'stroke', name: name || 'フチ', enabled: true, opacity: 100, width: w, position: 'outer', join: 'round', miterLimit: 4, paint });
  const shadow = (o) => Object.assign({ id: 'SH', type: 'shadow', name: 'シャドウ', enabled: true, opacity: 100, color: '#B4000000', angle: 90, distance: 6, blur: 8, spread: 0 }, o);
  const style = (id, name, stack, extra) => ({ id, name, style: Object.assign({ family: 'Yu Gothic UI', weight: 800, italic: false, tracking: 0, stack }, extra) });
  return {
    version: 1,
    seed: 1,
    styles: [
      style('d1', '白文字・黒フチ', [fill(solid('#FFFFFFFF')), stroke('S1', 8, solid('#FF000000')), shadow()]),
      style('d2', '黄文字・赤フチ・白フチ', [fill(solid('#FFFFE600')), stroke('S1', 7, solid('#FFE0141E')), stroke('S2', 16, solid('#FFFFFFFF'), '外フチ'), shadow({ distance: 8 })]),
      style('d3', 'ゴールドグラデ', [fill({ type: 'linear', angle: 90, stops: [{ pos: 0, color: '#FFFFF6C0' }, { pos: 0.45, color: '#FFFFD040' }, { pos: 0.55, color: '#FFD08A00' }, { pos: 1, color: '#FFFFE070' }] }),
        stroke('S1', 6, solid('#FF5A3000')), stroke('S2', 14, solid('#FFFFFFFF'), '外フチ'), shadow()]),
      style('d4', 'ポップピンク三重フチ', [fill(solid('#FFFFFFFF')), stroke('S1', 7, solid('#FFFF3C8C')), stroke('S2', 14, solid('#FFFFFFFF'), '中フチ'), stroke('S3', 20, solid('#FFFF3C8C'), '外フチ')], { weight: 900 }),
      style('d5', 'ストライプ塗り', [fill({ type: 'stripe', color: '#FFFFFFFF', color2: '#FF9ED8FF', angle: 45, size: 5, gap: 5 }), stroke('S1', 7, solid('#FF0A2A6A')), stroke('S2', 14, solid('#FFFFFFFF'), '外フチ')], { weight: 900 }),
      style('d6', 'ロングシャドウ', [fill(solid('#FFFFFFFF')), stroke('S1', 6, solid('#FF202020')),
        { id: 'LS', type: 'longShadow', name: 'ロングシャドウ', enabled: true, opacity: 100, angle: 45, length: 30, paint: solid('#FF303030') }], { weight: 900 }),
      style('d7', '黒座布団', [fill(solid('#FFFFFFFF')),
        { id: 'BG', type: 'background', name: '背景（座布団）', enabled: true, opacity: 100, padX: 36, padY: 14, radius: 0, paint: solid('#C8000000') }], { weight: 700 }),
      style('d8', 'ドット座布団', [fill(solid('#FF262626')), stroke('S1', 5, solid('#FFFFFFFF')),
        { id: 'BG', type: 'background', name: '背景（座布団）', enabled: true, opacity: 100, padX: 40, padY: 20, radius: 24, paint: { type: 'dots', color: '#FFFF6FA8', color2: '#FFFFE0EC', angle: 30, size: 12, gap: 22 } }], { weight: 900 }),
      style('d9', '内側シャドウ', [{ id: 'IS', type: 'innerShadow', name: '内側シャドウ', enabled: true, opacity: 100, color: '#A0003060', angle: 90, distance: 5, blur: 6, spread: 0 },
        fill({ type: 'linear', angle: 90, stops: [{ pos: 0, color: '#FFE6F7FF' }, { pos: 1, color: '#FF36A8FF' }] }), stroke('S1', 7, solid('#FF003A78')), stroke('S2', 14, solid('#FFFFFFFF'), '外フチ'), shadow()], { weight: 900 }),
    ],
    titles: [],
  };
}

// Presets as stored in presets.json, plus the factory samples this designer has not merged yet.
// folders: { style: [{ id, name }], title: [...] }. An item with no `folder` is not in a folder.
function loadPresets(saved) {
  const p = saved && saved.styles ? saved : defaultPresets();
  if (!Array.isArray(p.titles)) p.titles = [];
  p.folders = Object.assign({ style: [], title: [] }, p.folders);
  const seen = p.seed || 1;
  if (seen < SAMPLE_SEED) {
    for (const f of SAMPLE_FOLDERS) if (!p.folders.style.some((x) => x.id === f.id)) p.folders.style.push({ id: f.id, name: f.name });
    for (const s of SAMPLE_STYLES) {
      if (s.since > seen && !p.styles.some((x) => x.id === s.id)) p.styles.push({ id: s.id, name: s.name, folder: s.folder, style: clone(s.style) });
    }
    p.seed = SAMPLE_SEED;
    setTimeout(savePresets, 0); // state.presets is set by the caller by then
  }
  return p;
}

const thumbCache = new Map();

function thumbDocForStyle(p) {
  const st = p.style;
  // A cushion layer widens the telop: use a smaller sample text so it fits the thumbnail.
  const wide = st.stack.some((l) => l.type === 'background');
  return { format: 'NAKA-TDP', version: 2, frame: { width: 560, height: 240, vidScale: 30, vidSampleSize: 1, durationFrames: 1 },
    objects: [{ type: 'text', id: 'thumb', text: 'あア亜Ab', x: 280, y: 120, align: 'center', valign: 'middle', size: wide ? 72 : 104,
      family: st.family, weight: st.weight, italic: st.italic, tracking: st.tracking || 0, spans: [], kerning: [], stack: st.stack,
      light: Object.assign({}, LIGHT_DEFAULTS, st.light) }] };
}
function thumbDocForTitle(p) {
  // Thumbnails show the title at rest.
  const objects = p.objects.map((o) => Object.assign({}, o, { animIn: { type: 'none' }, animOut: { type: 'none' } }));
  return { format: 'NAKA-TDP', version: 2, frame: { width: p.width, height: p.height, vidScale: 30, vidSampleSize: 1, durationFrames: 1 }, objects };
}
// Only what is drawn counts, so renaming or moving a preset keeps its thumbnail.
const presetKey = (p) => p.id + ':' + JSON.stringify(p.style || p.objects).length;

function queueThumb(p) {
  const key = presetKey(p);
  if (thumbCache.has(key) || render.thumbKeys.has(key)) return;
  render.thumbKeys.add(key);
  const isStyle = !!p.style;
  const doc = isStyle ? thumbDocForStyle(p) : thumbDocForTitle(p);
  const w = isStyle ? 336 : 256, h = isStyle ? 144 : Math.round(256 * p.height / p.width);
  render.thumbs.push({ key, doc, w, h });
  pump();
}

function drawThumb(key) {
  const src = thumbCache.get(key);
  document.querySelectorAll(`canvas[data-key="${CSS.escape(key)}"]`).forEach((c) => {
    const g = c.getContext('2d');
    g.clearRect(0, 0, c.width, c.height);
    if (!src) return;
    const s = Math.min(c.width / src.width, c.height / src.height);
    g.drawImage(src, (c.width - src.width * s) / 2, (c.height - src.height * s) / 2, src.width * s, src.height * s);
  });
}

// ---- folders
const presetItems = () => (state.presetTab === 'style' ? state.presets.styles : state.presets.titles);
const presetFolders = () => state.presets.folders[state.presetTab];
const folderName = (id) => { const f = presetFolders().find((x) => x.id === id); return f ? f.name : ''; };
let dragPreset = null;

function selectFolder(id) {
  state.folderSel[state.presetTab] = id;
  buildPresets();
}

function moveToFolder(p, id) {
  if (id) p.folder = id; else delete p.folder;
  savePresets();
  buildPresets();
}

async function newFolder(thenMove) {
  const name = await promptDialog('フォルダ名', '');
  if (!name) return null;
  const f = { id: newId('D'), name };
  presetFolders().push(f);
  if (thenMove) thenMove.folder = f.id;
  state.folderSel[state.presetTab] = f.id;
  savePresets();
  buildPresets();
  return f;
}

async function renameFolder(f) {
  const name = await promptDialog('フォルダ名', f.name);
  if (!name) return;
  f.name = name;
  savePresets();
  buildPresets();
}

async function deleteFolder(f) {
  const items = presetItems().filter((p) => p.folder === f.id);
  const note = items.length ? `\n中の ${items.length} 件は「未分類」に戻ります（削除されません）。` : '';
  if (!(await confirmDialog(`フォルダ「${f.name}」を削除しますか？${note}`, '削除'))) return;
  for (const p of items) delete p.folder;
  const list = presetFolders();
  list.splice(list.indexOf(f), 1);
  if (state.folderSel[state.presetTab] === f.id) state.folderSel[state.presetTab] = '*';
  savePresets();
  buildPresets();
}

function buildFolderBar() {
  const bar = $('#folderBar');
  bar.replaceChildren();
  const items = presetItems(), folders = presetFolders();
  let sel = state.folderSel[state.presetTab];
  if (sel && !folders.some((f) => f.id === sel)) sel = state.folderSel[state.presetTab] = '*';
  const chip = (id, label, count, opt = {}) => {
    const b = el('button', { class: 'chip' + (sel === id ? ' on' : ''), title: opt.title || '', onclick: () => selectFolder(id) },
      label, el('span', { class: 'count', text: String(count) }));
    if (opt.menu) b.addEventListener('contextmenu', (e) => { e.preventDefault(); popupMenu(e.clientX, e.clientY, opt.menu); });
    if (opt.drop) {
      b.addEventListener('dragover', (e) => { if (dragPreset) { e.preventDefault(); b.classList.add('dropTarget'); } });
      b.addEventListener('dragleave', () => b.classList.remove('dropTarget'));
      b.addEventListener('drop', (e) => {
        e.preventDefault();
        b.classList.remove('dropTarget');
        if (dragPreset) moveToFolder(dragPreset, id);
        dragPreset = null;
      });
    }
    return b;
  };
  bar.append(chip('*', 'すべて', items.length, { title: 'すべてのフォルダの中身を表示' }));
  if (folders.length) {
    bar.append(chip('', '未分類', items.filter((p) => !p.folder).length, { drop: true, title: 'フォルダに入れていないもの。スタイルをドラッグして戻せます' }));
  }
  for (const f of folders) {
    bar.append(chip(f.id, f.name, items.filter((p) => p.folder === f.id).length, {
      drop: true,
      title: 'スタイルをドラッグして入れられます。右クリックで名前変更・削除',
      menu: [{ label: '名前を変更…', run: () => renameFolder(f) }, { label: 'フォルダを削除…', run: () => deleteFolder(f) }],
    }));
  }
  bar.append(el('button', { class: 'chip add', text: '＋ フォルダ', title: '新しいフォルダを作る', onclick: () => newFolder(null) }));
}

// ---- context menu
let menuEl = null;
function closeMenu() { if (menuEl) { menuEl.remove(); menuEl = null; } }
function popupMenu(x, y, entries) {
  closeMenu();
  const m = el('div', { class: 'menu' }, entries.map((it) => {
    if (it.sep) return el('div', { class: 'sep' });
    if (it.head) return el('div', { class: 'mh', text: it.head });
    return el('div', { class: 'mi' + (it.cur ? ' cur' : ''), text: it.label, onclick: () => { closeMenu(); it.run(); } });
  }));
  document.body.append(m);
  const r = m.getBoundingClientRect();
  m.style.left = Math.max(2, Math.min(x, innerWidth - r.width - 4)) + 'px';
  m.style.top = Math.max(2, Math.min(y, innerHeight - r.height - 4)) + 'px';
  menuEl = m;
}
document.addEventListener('pointerdown', (e) => { if (menuEl && !menuEl.contains(e.target)) closeMenu(); }, true);
document.addEventListener('keydown', (e) => { if (e.key === 'Escape') closeMenu(); });
window.addEventListener('blur', closeMenu);

function presetMenu(p, x, y) {
  const moves = [{ head: 'フォルダへ移動' }, { label: '未分類', cur: !p.folder, run: () => moveToFolder(p, '') }];
  for (const f of presetFolders()) moves.push({ label: f.name, cur: p.folder === f.id, run: () => moveToFolder(p, f.id) });
  moves.push({ label: '新しいフォルダへ…', run: () => newFolder(p) });
  popupMenu(x, y, [
    { label: '名前を変更…', run: async () => {
      const name = await promptDialog('名前', p.name);
      if (!name) return;
      p.name = name;
      savePresets();
      buildPresets();
    } },
    { sep: true }, ...moves, { sep: true },
    { label: '削除…', run: () => deletePreset(p) },
  ]);
}

async function deletePreset(p) {
  if (!(await confirmDialog(`「${p.name}」を削除しますか？`, '削除'))) return;
  const items = presetItems();
  items.splice(items.indexOf(p), 1);
  savePresets();
  buildPresets();
}

function buildPresets() {
  const list = $('#presetList');
  list.replaceChildren();
  if (!state.presets) return;
  buildFolderBar();
  $('#keepFontBox').hidden = $('#keepLightBox').hidden = state.presetTab !== 'style';
  const sel = state.folderSel[state.presetTab];
  const items = presetItems().filter((p) => sel === '*' || (sel === '' ? !p.folder : p.folder === sel));
  if (!items.length) {
    list.append(el('div', { class: 'hint', style: 'padding:8px', text: presetItems().length
      ? 'このフォルダは空です。スタイルをここへドラッグするか、右クリックの「フォルダへ移動」で入れられます。'
      : state.presetTab === 'style'
        ? '保存したスタイルがありません。'
        : '「現在の設定を保存」で、今のテロップ全体（全オブジェクト）をテンプレートとして保存できます。' }));
  }
  for (const p of items) {
    const key = presetKey(p);
    const c = el('canvas', { width: 336, height: 144, 'data-key': key });
    const del = el('button', { class: 'del small', text: '×', title: '削除', onclick: (e) => { e.stopPropagation(); deletePreset(p); } });
    const more = el('button', { class: 'more small', text: '⋯', title: 'メニュー（名前変更・フォルダへ移動）', onclick: (e) => {
      e.stopPropagation();
      const r = e.currentTarget.getBoundingClientRect();
      presetMenu(p, r.left, r.bottom);
    } });
    const label = p.name + (sel === '*' && p.folder && folderName(p.folder) ? `　[${folderName(p.folder)}]` : '');
    const card = el('div', { class: 'preset', draggable: 'true', title: p.name, onclick: () => applyPreset(p) },
      c, el('div', { class: 'label', text: label }), del, more);
    card.addEventListener('contextmenu', (e) => { e.preventDefault(); presetMenu(p, e.clientX, e.clientY); });
    card.addEventListener('dragstart', (e) => {
      dragPreset = p;
      e.dataTransfer.setData('text/plain', p.name);
      e.dataTransfer.effectAllowed = 'move';
      card.classList.add('dragging');
    });
    card.addEventListener('dragend', () => { dragPreset = null; card.classList.remove('dragging'); });
    list.append(card);
    if (thumbCache.has(key)) drawThumb(key); else queueThumb(p);
  }
}

// Keep the text's font settings (typeface, weight, italic, tracking) when applying a style.
const keepFont = $('#chkKeepFont');
try { keepFont.checked = localStorage.getItem('keepFont') === '1'; } catch { /* ignore */ }
keepFont.onchange = () => { try { localStorage.setItem('keepFont', keepFont.checked ? '1' : '0'); } catch { /* ignore */ } };
// Keep the text's own light (direction, height, strength, ambient, reflections) when applying a style.
const keepLight = $('#chkKeepLight');
try { keepLight.checked = localStorage.getItem('keepLight') === '1'; } catch { /* ignore */ }
keepLight.onchange = () => { try { localStorage.setItem('keepLight', keepLight.checked ? '1' : '0'); } catch { /* ignore */ } };

async function applyPreset(p) {
  if (p.style) {
    const o = obj();
    if (!o) { alertDialog('スタイルを適用するテキストを選択してください。'); return; }
    const st = clone(p.style);
    if (!keepFont.checked) { o.family = st.family; o.weight = st.weight; o.italic = !!st.italic; o.tracking = st.tracking || 0; }
    // Styles saved before the light was part of them have none: the text keeps its own light then.
    if (st.light && !keepLight.checked) o.light = Object.assign({}, LIGHT_DEFAULTS, st.light);
    o.stack = st.stack;
    state.layerSel = o.stack[0] ? o.stack[0].id : null;
    changed(true);
  } else {
    if (!(await confirmDialog(`今のテロップを「${p.name}」で置き換えますか？`, '置き換える'))) return;
    const f = state.doc.frame, sx = f.width / p.width, sy = f.height / p.height;
    state.doc.objects = clone(p.objects).map((o) => {
      o.id = newId('T');
      o.x *= sx; o.y *= sy; o.size *= sy;
      return o;
    });
    selectObject(state.doc.objects.length ? 0 : -1);
    changed(true);
  }
}

$('#btnSavePreset').onclick = async () => {
  // A new preset goes into the folder being shown (not for "all" / "not in a folder").
  const sel = state.folderSel[state.presetTab];
  const folder = sel && sel !== '*' ? sel : undefined;
  let p;
  if (state.presetTab === 'style') {
    const o = obj();
    if (!o) { alertDialog('保存するテキストを選択してください。'); return; }
    const name = await promptDialog('スタイル名', o.text.split('\n')[0].slice(0, 16));
    if (!name) return;
    p = { id: newId('S'), name, style: { family: o.family, weight: o.weight, italic: o.italic, tracking: o.tracking, stack: clone(o.stack),
      light: Object.assign({}, LIGHT_DEFAULTS, o.light) } };
    state.presets.styles.push(p);
  } else {
    const name = await promptDialog('テンプレート名', obj() ? obj().text.split('\n')[0].slice(0, 16) : 'テロップ');
    if (!name) return;
    p = { id: newId('P'), name, width: state.doc.frame.width, height: state.doc.frame.height, objects: clone(state.doc.objects) };
    state.presets.titles.push(p);
  }
  if (folder) p.folder = folder;
  savePresets();
  buildPresets();
};
document.querySelectorAll('.tab').forEach((t) => t.addEventListener('click', () => {
  state.presetTab = t.dataset.tab;
  document.querySelectorAll('.tab').forEach((x) => x.classList.toggle('active', x === t));
  buildPresets();
}));
function savePresets() { post({ type: 'savePresets', data: state.presets }); }

// ------------------------------------------------------------------ dialogs
function dialog(text, { input, okText, cancel = true, value = '' }) {
  return new Promise((resolve) => {
    const d = $('#dialog'), inp = $('#dialogInput');
    $('#dialogText').textContent = text;
    inp.hidden = !input;
    inp.value = value;
    $('#dialogOk').textContent = okText || 'OK';
    $('#dialogCancel').hidden = !cancel;
    d.hidden = false;
    if (input) { inp.focus(); inp.select(); } else $('#dialogOk').focus();
    const done = (ok) => {
      d.hidden = true;
      $('#dialogOk').onclick = $('#dialogCancel').onclick = inp.onkeydown = null;
      resolve(ok ? (input ? inp.value.trim() : true) : (input ? '' : false));
    };
    $('#dialogOk').onclick = () => done(true);
    $('#dialogCancel').onclick = () => done(false);
    inp.onkeydown = (e) => { if (e.key === 'Enter') done(true); if (e.key === 'Escape') done(false); };
  });
}
// A dialog with any number of buttons: choices = [{ label, value, primary }]. The last one is also what Escape does.
function choiceDialog(text, choices) {
  return new Promise((resolve) => {
    const d = $('#dialog'), box = d.querySelector('.buttons'), original = [...box.children];
    $('#dialogText').textContent = text;
    $('#dialogInput').hidden = true;
    const done = (value) => {
      document.removeEventListener('keydown', onKey, true);
      d.hidden = true;
      box.replaceChildren(...original);
      resolve(value);
    };
    const onKey = (e) => { if (e.key === 'Escape') { e.stopPropagation(); done(choices[choices.length - 1].value); } };
    document.addEventListener('keydown', onKey, true);
    const buttons = choices.map((c) => el('button', { class: c.primary ? 'primary' : '', text: c.label, onclick: () => done(c.value) }));
    box.replaceChildren(...buttons);
    d.hidden = false;
    (buttons.find((b) => b.classList.contains('primary')) || buttons[0]).focus();
  });
}
const promptDialog = (text, value) => dialog(text, { input: true, value });
const confirmDialog = (text, okText) => dialog(text, { okText });
const alertDialog = (text) => dialog(text, { cancel: false });

// ------------------------------------------------------------------ start
if (host) {
  post({ type: 'ready' });
} else {
  document.body.append(el('div', { style: 'position:fixed;left:50%;top:40%;transform:translate(-50%);color:#f88', text: 'このページはデザイナー（NAKA_TitleDesignerPro.exe）の中で開いてください。' }));
}
