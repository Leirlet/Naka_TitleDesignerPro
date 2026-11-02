// NAKA Title Designer Pro - factory sample styles (Japanese variety-show telop looks).
// Loaded before app.js; app.js merges them into the user's presets (see loadPresets).
'use strict';

// Bump SAMPLE_SEED when styles are added and give the new ones `since: <new seed>`. A presets.json
// written by an older designer gets only the styles newer than the seed it has seen, so styles the
// user deleted do not come back.
const SAMPLE_SEED = 2;

const SAMPLE_FOLDERS = [
  { id: 'fv-std', name: 'バラエティ 定番' },
  { id: 'fv-pop', name: 'バラエティ ポップ' },
  { id: 'fv-impact', name: 'バラエティ 衝撃' },
  { id: 'fv-band', name: 'バラエティ 帯・座布団' },
];

const SAMPLE_STYLES = (() => {
  const solid = (c) => ({ type: 'solid', color: c });
  const lin = (angle, ...stops) => ({ type: 'linear', angle, stops: stops.map(([pos, color]) => ({ pos, color })) });
  const base = { enabled: true, opacity: 100 };
  const fill = (paint) => Object.assign({ id: 'F', type: 'fill', name: '塗り', paint }, base);
  // Stroke rings are outer strokes: a wider one sits behind a narrower one, so widths grow with the index.
  const ring = (id, width, color, name) => Object.assign({ id, type: 'stroke', name: name || 'フチ', width, position: 'outer', join: 'round', miterLimit: 4, paint: solid(color) }, base);
  const rings = (...spec) => spec.map(([w, c], i) => ring('S' + (i + 1), w, c, i === 0 ? 'フチ' : i === 1 ? '中フチ' : '外フチ'));
  const shadow = (o) => Object.assign({ id: 'SH', type: 'shadow', name: 'シャドウ', color: '#A0000000', angle: 60, distance: 6, blur: 6, spread: 0 }, base, o);
  const glow = (id, color, blur, spread) => shadow({ id, name: '光彩', color, distance: 0, blur, spread });
  const band = (padX, padY, radius, paint) => Object.assign({ id: 'BG', type: 'background', name: '背景（座布団）', padX, padY, radius, paint }, base);
  const longShadow = (angle, length, color) => Object.assign({ id: 'LS', type: 'longShadow', name: 'ロングシャドウ', angle, length, paint: solid(color) }, base);

  const NOTO = ['Noto Sans JP', 900];
  const ROUND = ['Rounded Mplus 1c', 800];
  const MOCHI = ['Mochiy Pop One', 400];
  const MINCHO = ['Zen Old Mincho', 900];
  const sample = (id, name, folder, font, stack, extra) => ({
    id: 'fv-' + id, name, folder, since: 2,
    style: Object.assign({ family: font[0], weight: font[1], italic: false, tracking: 0, stack }, extra),
  });
  const WHITE = '#FFFFFFFF';
  const wide = { tracking: 2 };

  return [
    // ---- 定番: 文字色 + 色フチ + 白フチ + 濃い縁
    sample('std-blue', '白文字・青フチ', 'fv-std', NOTO, [fill(solid(WHITE)), ...rings([8, '#FF1E5BD8'], [15, WHITE], [19, '#FF0B1F5C']), shadow()]),
    sample('std-red', '白文字・赤フチ', 'fv-std', NOTO, [fill(solid(WHITE)), ...rings([8, '#FFE0141E'], [15, WHITE], [19, '#FF5A0008']), shadow()]),
    sample('std-green', '白文字・緑フチ', 'fv-std', NOTO, [fill(solid(WHITE)), ...rings([8, '#FF12A24A'], [15, WHITE], [19, '#FF064A1E']), shadow()]),
    sample('std-orange', '白文字・橙フチ', 'fv-std', NOTO, [fill(solid(WHITE)), ...rings([8, '#FFFF7A00'], [15, WHITE], [19, '#FF6A2A00']), shadow()]),
    sample('std-purple', '白文字・紫フチ', 'fv-std', NOTO, [fill(solid(WHITE)), ...rings([8, '#FF8A3CE0'], [15, WHITE], [19, '#FF2E0A62']), shadow()]),
    sample('std-pink', '白文字・ピンクフチ', 'fv-std', NOTO, [fill(solid(WHITE)), ...rings([8, '#FFFF2E86'], [15, WHITE], [19, '#FF6A0A3A']), shadow()]),
    sample('std-cyan', '白文字・水色フチ', 'fv-std', NOTO, [fill(solid(WHITE)), ...rings([8, '#FF18A8E8'], [15, WHITE], [19, '#FF05466A']), shadow()]),
    sample('std-yellow', '黄文字・黒白フチ', 'fv-std', NOTO, [fill(lin(90, [0, '#FFFFFFB0'], [1, '#FFFFE600'])), ...rings([9, '#FF000000'], [16, WHITE], [20, '#FF000000']), shadow()]),
    sample('std-black-edge', '白文字・黒フチ（太）', 'fv-std', NOTO, [fill(solid(WHITE)), ring('S1', 12, '#FF000000'), shadow({ distance: 6, blur: 0, color: '#FF000000' })]),
    sample('std-black-text', '黒文字・白フチ', 'fv-std', NOTO, [fill(solid('#FF111111')), ...rings([10, WHITE], [14, '#FF555555']), shadow()]),

    // ---- ポップ: グラデ・模様・厚い縁
    sample('pop-rainbow', 'レインボー', 'fv-pop', ROUND, [
      fill(lin(0, [0, '#FFFF3B30'], [0.2, '#FFFF9500'], [0.4, '#FFFFE600'], [0.6, '#FF34C759'], [0.8, '#FF2D8CEB'], [1, '#FFAF52DE'])),
      ...rings([8, WHITE], [13, '#FF303030']), shadow()], wide),
    sample('pop-pink', 'ピンク→オレンジ', 'fv-pop', ROUND, [
      fill(lin(90, [0, '#FFFF5FA2'], [1, '#FFFFA03A'])), ...rings([8, WHITE], [15, '#FF7A1E50']), shadow()], wide),
    sample('pop-sky', '水色→青', 'fv-pop', ROUND, [
      fill(lin(90, [0, '#FFB8F0FF'], [1, '#FF1E90FF'])), ...rings([6, '#FF0A3C8C'], [13, WHITE], [17, '#FF0A3C8C']), shadow()], wide),
    sample('pop-food', '黄→橙・赤フチ（食レポ）', 'fv-pop', ROUND, [
      fill(lin(90, [0, '#FFFFF27A'], [1, '#FFFF8A00'])), ...rings([7, '#FFC8140A'], [14, WHITE], [18, '#FF7A0A00']), shadow()], wide),
    sample('pop-mint', 'ミント→緑', 'fv-pop', ROUND, [
      fill(lin(90, [0, '#FFEFFFD8'], [1, '#FF3CD27A'])), ...rings([7, '#FF0A7A3C'], [14, WHITE]), shadow()], wide),
    sample('pop-lavender', 'ラベンダー→紫', 'fv-pop', ROUND, [
      fill(lin(90, [0, '#FFFFE0FF'], [1, '#FFB470FF'])), ...rings([7, '#FF5A1EA0'], [14, WHITE]), shadow()], wide),
    sample('pop-sticker-red', 'ステッカー（赤）', 'fv-pop', MOCHI, [
      fill(solid('#FFFF3B3B')), ring('S1', 22, WHITE, '白フチ'), shadow({ distance: 6, blur: 10, color: '#70000000' })]),
    sample('pop-sticker-sky', 'ステッカー（水色）', 'fv-pop', MOCHI, [
      fill(solid('#FF1E90FF')), ring('S1', 22, WHITE, '白フチ'), shadow({ distance: 6, blur: 10, color: '#70000000' })]),
    sample('pop-dots', 'ドット柄', 'fv-pop', ROUND, [
      fill({ type: 'dots', color: WHITE, color2: '#FFFF4F9A', angle: 30, size: 7, gap: 14 }), ...rings([7, '#FFC21E6E'], [14, WHITE]), shadow()], wide),
    sample('pop-stripe', 'ストライプ（赤白）', 'fv-pop', ROUND, [
      fill({ type: 'stripe', color: WHITE, color2: '#FFE0141E', angle: 45, size: 8, gap: 8 }), ...rings([6, '#FF7A0A10'], [13, WHITE]), shadow()], wide),

    // ---- 衝撃: 光彩・金属・ずらし影
    sample('imp-red', '衝撃・赤', 'fv-impact', NOTO, [
      fill(lin(90, [0, '#FFFF6A5A'], [1, '#FFD00010'])), ...rings([7, WHITE], [15, '#FF000000']), shadow({ distance: 8, blur: 2, color: '#C8000000' })]),
    sample('imp-offset', 'ずらし影・黄', 'fv-impact', NOTO, [
      fill(solid('#FFFFE600')), ring('S1', 6, '#FF000000'), longShadow(45, 14, '#FFE0141E')]),
    sample('imp-gold', '金文字・重厚', 'fv-impact', NOTO, [
      fill(lin(90, [0, '#FFFFF6C0'], [0.45, '#FFFFD040'], [0.55, '#FFD08A00'], [1, '#FFFFE070'])), ...rings([5, '#FF4A2400'], [11, '#FF000000']), shadow({ distance: 8, blur: 6, color: '#C8000000' })]),
    sample('imp-silver', '銀文字', 'fv-impact', NOTO, [
      fill(lin(90, [0, WHITE], [0.45, '#FFC8D0DC'], [0.55, '#FF8A94A4'], [1, '#FFE8EEF6'])), ...rings([5, '#FF1E2838'], [11, WHITE], [15, '#FF1E2838']), shadow()]),
    sample('imp-fire', '炎', 'fv-impact', NOTO, [
      fill(lin(90, [0, '#FFFFF59A'], [0.4, '#FFFFB000'], [0.75, '#FFFF4A00'], [1, '#FFD00000'])), ring('S1', 6, '#FF4A0A00'), glow('SH', '#FFFF7A00', 18, 4)]),
    sample('imp-ice', '氷', 'fv-impact', NOTO, [
      fill(lin(90, [0, WHITE], [0.5, '#FF8AE8FF'], [1, '#FF3AA0FF'])), ...rings([6, '#FF0A3C78'], [12, WHITE]), glow('SH', '#FF40C8FF', 16, 3)]),
    sample('imp-suspense', 'サスペンス', 'fv-impact', MINCHO, [
      fill(lin(90, [0, '#FFF2F2F2'], [1, '#FFC0A0A0'])), ring('S1', 6, '#FF000000'), glow('SH', '#FFB00000', 16, 3)], { tracking: 4 }),
    sample('imp-neon-pink', 'ネオン・ピンク', 'fv-impact', ROUND, [
      fill(solid('#FFFFEAF4')), ring('S1', 4, '#FFFF3C9A'), glow('SH', '#FFFF3C9A', 14, 2), glow('SH2', '#80FF3C9A', 36, 6)], wide),
    sample('imp-neon-cyan', 'ネオン・水色', 'fv-impact', ROUND, [
      fill(solid('#FFEAFCFF')), ring('S1', 4, '#FF18D8FF'), glow('SH', '#FF18D8FF', 14, 2), glow('SH2', '#8018D8FF', 36, 6)], wide),

    // ---- 帯・座布団
    sample('band-black', '黒帯・白文字', 'fv-band', NOTO, [fill(solid(WHITE)), band(48, 16, 0, solid('#FF111111'))]),
    sample('band-red', '赤帯・白文字', 'fv-band', NOTO, [fill(solid(WHITE)), band(48, 16, 0, solid('#FFD3101A'))]),
    sample('band-yellow', '黄帯・黒文字', 'fv-band', NOTO, [fill(solid('#FF111111')), band(48, 16, 0, solid('#FFFFE000'))]),
    sample('band-blue', '青帯・白文字', 'fv-band', NOTO, [fill(solid(WHITE)), band(48, 16, 0, lin(0, [0, '#FF1E5BD8'], [1, '#FF18A8E8']))]),
    sample('band-fade', 'フェード帯', 'fv-band', NOTO, [fill(solid(WHITE)), shadow({ distance: 3, blur: 4, color: '#C8000000' }),
      band(120, 16, 0, lin(0, [0, '#00000000'], [0.3, '#D8000000'], [0.7, '#D8000000'], [1, '#00000000']))], { weight: 900 }),
    sample('band-white', '白座布団・黒文字', 'fv-band', NOTO, [fill(solid('#FF222222')), band(44, 16, 30, solid(WHITE)), shadow({ distance: 6, blur: 10, color: '#70000000' })]),
    sample('band-pink', 'ピンク座布団・ずらし影', 'fv-band', ROUND, [fill(solid('#FFE0286E')), band(44, 16, 40, solid('#FFFFE0EC')), shadow({ distance: 8, blur: 0, angle: 45, color: '#FFFF6FA8' })], wide),
    sample('band-green', '緑ラベル', 'fv-band', NOTO, [fill(solid(WHITE)), ring('S1', 4, '#FF064A1E'), band(44, 16, 24, solid('#FF12A24A')), shadow({ distance: 5, blur: 6 })]),
    sample('band-hazard', 'ドッキリ警告帯', 'fv-band', NOTO, [fill(solid(WHITE)), ring('S1', 10, '#FF000000'),
      band(44, 14, 8, { type: 'stripe', color: '#FF111111', color2: '#FFFFD400', angle: 45, size: 18, gap: 18 })]),
  ];
})();
