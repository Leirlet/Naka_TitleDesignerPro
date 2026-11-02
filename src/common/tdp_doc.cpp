#include "tdp_doc.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>

namespace tdp {

using json::Value;

namespace {

const char* kPaintTypes[] = { "solid", "linear", "radial", "stripe", "dots", "texture" };
const char* kLayerTypes[] = { "fill", "stroke", "shadow", "innerShadow", "longShadow", "background", "extrude", "texture" };
const char* kTurnModes[] = { "none", "spin", "sway" };
const char* kLightMotions[] = { "none", "orbit", "sweep", "pass" };
const char* kKernModes[] = { "metrics", "none", "palt" };
const char* kTexFits[] = { "cover", "contain", "stretch", "tile" };
const char* kTexAreas[] = { "text", "frame" };
const char* kTexBlends[] = { "normal", "multiply", "screen", "overlay", "add", "lighten", "darken", "softLight" };
const char* kStrokePositions[] = { "outer", "center", "inner" };
const char* kJoins[] = { "miter", "round", "bevel" };
const char* kBevels[] = { "none", "flat", "round", "cove" };
const char* kMaterials[] = { "none", "lit" };
const char* kEnvStyles[] = { "soft", "studio" };
const char* kHAligns[] = { "left", "center", "right" };
const char* kVAligns[] = { "top", "middle", "bottom" };

template <typename E, size_t N>
E EnumFrom(const Value& v, const char* (&names)[N], E fallback)
{
	for (size_t i = 0; i < N; ++i)
		if (v.asString() == names[i]) return (E)i;
	return fallback;
}

template <typename E, size_t N>
const char* EnumName(E e, const char* (&names)[N])
{
	size_t i = (size_t)e;
	return i < N ? names[i] : names[0];
}

float F(const Value& v, const char* key, float fallback) { return v[key].asFloat(fallback); }

const wchar_t* LayerDefaultName(LayerType t)
{
	switch (t) {
	case LayerType::Fill: return L"塗り";
	case LayerType::Stroke: return L"フチ";
	case LayerType::Shadow: return L"シャドウ";
	case LayerType::InnerShadow: return L"内側シャドウ";
	case LayerType::LongShadow: return L"ロングシャドウ";
	case LayerType::Background: return L"背景";
	case LayerType::Extrude: return L"3D";
	case LayerType::Texture: return L"テクスチャ";
	}
	return L"";
}

const char* kAnimTypes[] = { "none", "fade", "typewriter", "pop", "zoom", "drop", "rise", "slide", "spin", "flip3d", "flyz", "hinge", "tumble", "grow" };
const char* kAnimUnits[] = { "char", "line", "all" };
const char* kAnimOrders[] = { "forward", "backward", "centerOut", "edgesIn", "random" };
const char* kAnimEases[] = { "auto", "linear", "out", "inOut", "back", "bounce", "elastic" };
const char* kAnimDirs[] = { "left", "right", "up", "down" };

// Phase 0 / early Phase 1 files had a whole-object fade-in in frames.
AnimSpec FadeInAnim(int frames)
{
	AnimSpec a;
	if (frames <= 0) return a;
	a.type = AnimType::Fade;
	a.unit = AnimUnit::All;
	a.duration = (float)frames;
	a.stagger = 0;
	return a;
}

// ---------------------------------------------------------------- v1 (Phase 0 key=value)
bool LoadV1(const std::string& bytes, Doc& out)
{
	std::map<std::wstring, std::wstring> kv;
	std::wstring all = json::FromUtf8(bytes);
	size_t pos = 0;
	while (pos <= all.size()) {
		size_t eol = all.find(L'\n', pos);
		if (eol == std::wstring::npos) eol = all.size();
		std::wstring line = all.substr(pos, eol - pos);
		if (!line.empty() && line.back() == L'\r') line.pop_back();
		size_t eq = line.find(L'=');
		if (eq != std::wstring::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
		pos = eol + 1;
	}
	if (kv[L"format"] != L"NAKA-TDP") return false;
	auto num = [&](const wchar_t* k, double fb) { auto it = kv.find(k); return it == kv.end() ? fb : _wtof(it->second.c_str()); };
	auto col = [&](const wchar_t* k, uint32_t fb) { auto it = kv.find(k); return it == kv.end() ? fb : (uint32_t)wcstoul(it->second.c_str(), nullptr, 16); };
	auto unescape = [](const std::wstring& v) {
		std::wstring r;
		for (size_t i = 0; i < v.size(); ++i) {
			if (v[i] == L'\\' && i + 1 < v.size()) { wchar_t n = v[++i]; r += (n == L'n') ? L'\n' : n; }
			else r += v[i];
		}
		return r;
	};

	Doc d = MakeDefaultDoc((int)num(L"width", 1920), (int)num(L"height", 1080),
		(int)num(L"vid_scale", 30000), (int)num(L"vid_sample_size", 1001));
	d.durationFrames = (int)num(L"duration_frames", 150);
	TextObject& t = d.objects[0];
	if (kv.count(L"text")) t.text = unescape(kv[L"text"]);
	if (kv.count(L"font")) t.family = unescape(kv[L"font"]);
	t.size = (float)num(L"font_size", 120);
	t.weight = num(L"bold", 1) != 0 ? 700 : 400;
	t.animIn = FadeInAnim((int)num(L"fade_in_frames", 15));
	t.stack.clear();
	Layer fill = MakeLayer(LayerType::Fill);
	fill.paint.color = col(L"fill_color", 0xFFFFFFFF);
	t.stack.push_back(fill);
	float sw = (float)num(L"stroke_width", 12);
	if (sw > 0) {
		Layer stroke = MakeLayer(LayerType::Stroke);
		stroke.width = sw;
		stroke.paint.color = col(L"stroke_color", 0xFF000000);
		t.stack.push_back(stroke);
	}
	out = d;
	return true;
}

} // namespace

// ---------------------------------------------------------------- colours / ids
std::string ColorToString(uint32_t argb)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "#%08X", argb);
	return buf;
}

uint32_t ColorFromString(const std::string& s, uint32_t fallback)
{
	if (s.size() < 2 || s[0] != '#') return fallback;
	char* end = nullptr;
	unsigned long v = strtoul(s.c_str() + 1, &end, 16);
	if (*end != '\0') return fallback;
	if (s.size() == 7) return 0xFF000000u | (uint32_t)v; // #RRGGBB
	if (s.size() == 9) return (uint32_t)v;               // #AARRGGBB
	return fallback;
}

std::string NewId(const char* prefix)
{
	static std::atomic<unsigned> counter{ 0 };
	char buf[64];
	snprintf(buf, sizeof(buf), "%s%llx%x", prefix, (unsigned long long)GetTickCount64(), counter++);
	return buf;
}

Layer MakeLayer(LayerType type)
{
	Layer l;
	l.id = NewId("L");
	l.type = type;
	l.name = LayerDefaultName(type);
	if (type != LayerType::Extrude) l.bevel = BevelShape::None; // the round default is for 3D
	switch (type) {
	case LayerType::Fill:
		l.paint.color = 0xFFFFFFFF;
		break;
	case LayerType::Stroke:
		l.paint.color = 0xFF000000;
		l.width = 8;
		break;
	case LayerType::Shadow:
		l.color = 0xB4000000;
		l.distance = 8;
		l.blur = 8;
		break;
	case LayerType::InnerShadow:
		l.color = 0x96000000;
		l.distance = 4;
		l.blur = 6;
		break;
	case LayerType::LongShadow:
		l.paint.color = 0xFF404040;
		l.angle = 45;
		l.length = 24;
		break;
	case LayerType::Background:
		l.paint.color = 0xC8000000;
		l.padX = 32;
		l.padY = 16;
		l.radius = 12;
		break;
	case LayerType::Extrude:
		break; // defaults in Layer
	case LayerType::Texture:
		l.tex.blend = TexBlend::Screen; // particles / light: the usual use of an overlay
		break;
	}
	return l;
}

TextObject MakeTextObject(const Doc& doc)
{
	TextObject t;
	t.id = NewId("T");
	t.name = L"テキスト";
	t.x = doc.width * 0.5f;
	t.y = doc.height * 0.82f;
	t.size = doc.height / 9.0f;
	Layer fill = MakeLayer(LayerType::Fill);
	Layer stroke = MakeLayer(LayerType::Stroke);
	stroke.width = t.size * 0.07f;
	Layer shadow = MakeLayer(LayerType::Shadow);
	t.stack = { fill, stroke, shadow };
	return t;
}

Doc MakeDefaultDoc(int width, int height, int vidScale, int vidSampleSize)
{
	Doc d;
	if (width > 0 && height > 0) { d.width = width; d.height = height; }
	if (vidScale > 0 && vidSampleSize > 0) { d.vidScale = vidScale; d.vidSampleSize = vidSampleSize; }
	d.durationFrames = (int)((5LL * d.vidScale + d.vidSampleSize / 2) / d.vidSampleSize); // 5 seconds
	d.objects.push_back(MakeTextObject(d));
	return d;
}

float ObjectOpacity(const TextObject& obj)
{
	float o = obj.opacity / 100.0f;
	return o < 0 ? 0 : (o > 1 ? 1 : o);
}

// ---------------------------------------------------------------- animation
namespace {

constexpr float kPiF = 3.14159265358979f;

float Clamp01(float t) { return t < 0 ? 0 : (t > 1 ? 1 : t); }

float EaseOutBounce(float t)
{
	const float n = 7.5625f, d = 2.75f;
	if (t < 1 / d) return n * t * t;
	if (t < 2 / d) { t -= 1.5f / d; return n * t * t + 0.75f; }
	if (t < 2.5f / d) { t -= 2.25f / d; return n * t * t + 0.9375f; }
	t -= 2.625f / d;
	return n * t * t + 0.984375f;
}

float Ease(AnimEase e, float t)
{
	t = Clamp01(t);
	switch (e) {
	case AnimEase::Linear: return t;
	case AnimEase::InOut: return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2;
	case AnimEase::Back: { const float c1 = 1.70158f, c3 = c1 + 1; return 1 + c3 * std::pow(t - 1, 3.0f) + c1 * std::pow(t - 1, 2.0f); }
	case AnimEase::Bounce: return EaseOutBounce(t);
	case AnimEase::Elastic:
		if (t <= 0 || t >= 1) return t;
		return std::pow(2.0f, -10 * t) * std::sin((t * 10 - 0.75f) * (2 * kPiF / 3)) + 1;
	case AnimEase::Out:
	case AnimEase::Auto:
	default: return 1 - std::pow(1 - t, 3.0f);
	}
}

AnimEase DefaultEase(AnimType t)
{
	switch (t) {
	case AnimType::Pop: return AnimEase::Back;
	case AnimType::Drop: return AnimEase::Bounce;
	case AnimType::Rise: return AnimEase::Back;
	case AnimType::Fade: return AnimEase::Linear;
	default: return AnimEase::Out;
	}
}

float DefaultAmount(AnimType t)
{
	switch (t) {
	case AnimType::Pop: return 0;      // start scale %
	case AnimType::Zoom: return 300;   // start scale %
	case AnimType::Drop: return 160;   // px above
	case AnimType::Rise: return 80;    // px below
	case AnimType::Slide: return 120;  // px
	case AnimType::Spin: return 360;   // degrees
	case AnimType::Flip3D: return 90;  // degrees
	case AnimType::FlyZ: return 800;   // px behind / in front
	case AnimType::Hinge: return 90;   // degrees
	case AnimType::Tumble: return 360; // degrees
	default: return 0;
	}
}

// Position of unit `index` in the playing order.
int OrderSlot(const AnimSpec& a, int index, int count)
{
	if (count <= 1) return 0;
	switch (a.order) {
	case AnimOrder::Backward: return count - 1 - index;
	case AnimOrder::CenterOut: {
		const float c = (count - 1) * 0.5f;
		return (int)std::floor(std::fabs(index - c) + 0.01f);
	}
	case AnimOrder::EdgesIn: {
		const float c = (count - 1) * 0.5f;
		return (int)std::floor(c - std::fabs(index - c) + 0.01f);
	}
	case AnimOrder::Random: {
		// Rank of this unit in a seeded shuffle.
		auto key = [&](int i) {
			uint32_t h = (uint32_t)i * 2654435761u ^ (uint32_t)a.seed * 40503u;
			h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
			return h;
		};
		const uint32_t mine = key(index);
		int rank = 0;
		for (int i = 0; i < count; ++i) if (key(i) < mine || (key(i) == mine && i < index)) ++rank;
		return rank;
	}
	default: return index;
	}
}

int SlotCount(const AnimSpec& a, int count)
{
	int m = 0;
	for (int i = 0; i < count; ++i) m = std::max(m, OrderSlot(a, i, count));
	return m + 1;
}

// Applies the motion of `a` at progress raw (0 = start of the in-motion, 1 = at rest).
// Out-motions are the in-motion backwards, except drop, which falls down out of the frame.
void ApplyMotion(const AnimSpec& a, float raw, bool out, UnitState& s)
{
	if (a.type == AnimType::None) return;
	raw = Clamp01(raw);
	if (a.type == AnimType::Typewriter) {
		if (raw <= 0) s.opacity = 0;
		return;
	}
	AnimEase ease = a.ease == AnimEase::Auto ? DefaultEase(a.type) : a.ease;
	if (out && a.type == AnimType::Drop && a.ease == AnimEase::Auto) ease = AnimEase::Out; // a bounce backwards looks broken
	const float p = Ease(ease, raw);
	const float amount = a.amount != 0 ? a.amount : DefaultAmount(a.type);
	switch (a.type) {
	case AnimType::Fade: s.opacity *= p; return;
	case AnimType::Pop:
	case AnimType::Zoom: {
		const float from = amount / 100.0f;
		s.scale *= from + (1 - from) * p;
		break;
	}
	case AnimType::Drop: s.dy += (out ? amount : -amount) * (1 - p); break;
	case AnimType::Rise: s.dy += amount * (1 - p); break;
	case AnimType::Slide: {
		const float d = amount * (1 - p);
		if (a.dir == AnimDir::Left) s.dx -= d;
		else if (a.dir == AnimDir::Right) s.dx += d;
		else if (a.dir == AnimDir::Up) s.dy -= d;
		else s.dy += d;
		break;
	}
	case AnimType::Spin:
		s.rotation += amount * (1 - p);
		s.scale *= p;
		break;
	case AnimType::Flip3D: {
		const float d = amount * (1 - p);
		if (a.dir == AnimDir::Left) s.rotY += d;
		else if (a.dir == AnimDir::Right) s.rotY -= d;
		else if (a.dir == AnimDir::Up) s.rotX -= d;
		else s.rotX += d;
		break;
	}
	case AnimType::FlyZ: {
		const float d = amount * (1 - p);
		s.dz += a.dir == AnimDir::Right ? -d : d;
		break;
	}
	case AnimType::Hinge: {
		// Turns around the edge named by dir; positive rotX moves the bottom away, positive rotY the right side.
		const float d = amount * (1 - p);
		if (a.dir == AnimDir::Left) { s.rotY += d; s.px = -0.5f; }
		else if (a.dir == AnimDir::Right) { s.rotY -= d; s.px = 0.5f; }
		else if (a.dir == AnimDir::Up) { s.rotX += d; s.py = -0.5f; }
		else { s.rotX -= d; s.py = 0.5f; }
		break;
	}
	case AnimType::Tumble: {
		const float d = amount * (1 - p);
		const float sy = (a.dir == AnimDir::Left || a.dir == AnimDir::Up) ? 1.0f : -1.0f;
		const float sx = (a.dir == AnimDir::Left || a.dir == AnimDir::Down) ? 1.0f : -1.0f;
		s.rotY += sy * d;
		s.rotX += sx * d * 0.6f;
		break;
	}
	case AnimType::Grow:
		s.depth *= p;
		return; // the extrusion grows in place: no fade
	default: break;
	}
	// Fade with the motion, but faster than it so overshoot / bounce happen fully visible.
	if (a.fade) s.opacity *= Clamp01(raw * 3);
}

float SpanFrames(const AnimSpec& a, int count)
{
	return a.delay + (SlotCount(a, count) - 1) * a.stagger + std::max(a.duration, 0.0f);
}

} // namespace

bool HasAnimation(const TextObject& obj)
{
	return obj.animIn.type != AnimType::None || obj.animOut.type != AnimType::None;
}

int Extrude3DIndex(const TextObject& obj)
{
	for (size_t i = 0; i < obj.stack.size(); ++i)
		if (obj.stack[i].type == LayerType::Extrude && obj.stack[i].enabled && obj.stack[i].opacity > 0) return (int)i;
	return -1;
}

bool HasSurface(const Layer& l)
{
	if (l.type != LayerType::Fill && l.type != LayerType::Stroke) return false;
	return l.material == Material::Lit || (l.bevel != BevelShape::None && l.bevelWidth > 0 && l.bevelHeight > 0)
		|| (!l.bump.path.empty() && l.bumpDepth != 0);
}

Light AnimatedLight(const Light& l, int frame, float fps)
{
	Light out = l;
	const float t = fps > 0 ? (float)frame / fps : 0.0f; // seconds
	const float period = std::max(l.period, 0.05f);
	switch (l.motion) {
	case LightMotion::Orbit:
		out.angle = l.angle + 360.0f * t / period;
		break;
	case LightMotion::Sweep:
		out.angle = l.angle + l.range * 0.5f * std::sin(2 * kPiF * t / period);
		break;
	case LightMotion::Pass: {
		const float u = Clamp01(l.duration > 0 ? ((float)frame - l.start) / l.duration : ((float)frame >= l.start ? 1.0f : 0.0f));
		const float e = u * u * (3 - 2 * u); // smooth in and out
		out.angle = l.angle - l.range * 0.5f + l.range * e;
		break;
	}
	case LightMotion::None:
		break;
	}
	return out;
}

void TurnOf(const Layer& l, int frame, float fps, float& dRotX, float& dRotY)
{
	dRotX = dRotY = 0;
	if (l.turn == TurnMode::None) return;
	const float t = fps > 0 ? (float)frame / fps : 0.0f;
	float a;
	if (l.turn == TurnMode::Spin) a = l.turnPhase + l.turnSpeed * t;
	else a = l.turnAmount * std::sin(2 * kPiF * t / std::max(l.turnPeriod, 0.05f) + l.turnPhase * kPiF / 180.0f);
	(l.turnX ? dRotX : dRotY) = a;
}

int TextureFrameIndex(const Texture& t, int frame, float fps, int count, double texFps)
{
	if (count <= 1) return 0;
	const double sec = (double)t.startTime + (fps > 0 ? frame / (double)fps : 0.0) * t.speed / 100.0;
	long long i = (long long)std::floor(sec * (texFps > 0 ? texFps : 30.0));
	if (t.loop) {
		i %= count;
		if (i < 0) i += count;
	} else {
		i = std::clamp<long long>(i, 0, count - 1);
	}
	return (int)i;
}

std::vector<std::pair<int, int>> TcyRanges(const TextObject& obj)
{
	std::vector<std::pair<int, int>> out;
	if (!obj.vertical) return out;
	const int n = (int)obj.text.size();
	auto isBreak = [&](int i) { return obj.text[i] == L'\n' || obj.text[i] == L'\r'; };
	// Marked ranges: every maximal run of marked characters (within a line) is one group.
	std::vector<bool> marked(n, false);
	for (const Span& s : obj.spans)
		if (s.tcy)
			for (int i = std::max(s.start, 0); i < s.start + s.length && i < n; ++i) marked[i] = !isBreak(i);
	// Automatic: half-width digit runs up to tcyDigits long, and "!!" / "!?" / "?!" / "??".
	auto isDigit = [&](int i) { return obj.text[i] >= L'0' && obj.text[i] <= L'9'; };
	auto isMark = [&](int i) { return obj.text[i] == L'!' || obj.text[i] == L'?'; };
	auto attaches = [&](int i) {
		return !marked[i] && !isBreak(i) && obj.tcyChars.find(obj.text[i]) != std::wstring::npos;
	};
	int lastEnd = 0; // end of the previous group: attached characters are not shared
	for (int i = 0; i < n;) {
		if (marked[i]) {
			int j = i + 1;
			while (j < n && marked[j]) ++j;
			out.push_back({ i, j - i });
			lastEnd = i = j;
			continue;
		}
		if (obj.tcyDigits > 0 && (isDigit(i) || isMark(i))) {
			const bool digits = isDigit(i);
			int j = i + 1;
			while (j < n && !marked[j] && (digits ? isDigit(j) : isMark(j))) ++j;
			if (digits ? j - i <= obj.tcyDigits : j - i == 2) {
				int a = i, b = j;
				if (digits) {
					while (a > lastEnd && attaches(a - 1)) --a;
					while (b < n && attaches(b)) ++b;
				}
				out.push_back({ a, b - a });
				j = b;
				lastEnd = j;
			}
			i = j;
			continue;
		}
		++i;
	}
	return out;
}

UnitState EvalAnim(const AnimSpec& a, bool out, int index, int count, int frame, int durationFrames)
{
	UnitState s;
	if (a.type == AnimType::None) return s;
	const float f = (float)frame;
	if (!out) {
		const float start = a.delay + OrderSlot(a, index, count) * a.stagger;
		const float d = std::max(a.duration, 0.0f);
		// Typewriter: the unit appears on its start frame.
		const float raw = a.type == AnimType::Typewriter ? (f >= start ? 1.0f : 0.0f) : (d > 0 ? (f - start) / d : (f >= start ? 1.0f : 0.0f));
		ApplyMotion(a, raw, false, s);
	} else {
		// Played backwards from the clip end: the last frame shows the start of the in-motion.
		const float end = (float)durationFrames - a.delay;            // all units gone at this frame
		const float total = SpanFrames(a, count) - a.delay;
		const float outStart = end - total;
		const float start = outStart + OrderSlot(a, index, count) * a.stagger;
		const float d = std::max(a.duration, 0.0f);
		const float t = a.type == AnimType::Typewriter ? (f + 1 >= start + d ? 0.0f : 1.0f)
			: (d > 0 ? 1 - (f + 1 - start) / d : (f + 1 >= start ? 0.0f : 1.0f));
		ApplyMotion(a, t, true, s);
	}
	return s;
}

// ---------------------------------------------------------------- JSON
Value TextureToJson(const Texture& t)
{
	Value v = Value::Object();
	v.set("path", t.path);
	v.set("area", EnumName(t.area, kTexAreas));
	v.set("fit", EnumName(t.fit, kTexFits));
	v.set("scale", t.scale);
	v.set("offsetX", t.offsetX);
	v.set("offsetY", t.offsetY);
	v.set("rotation", t.rotation);
	v.set("blend", EnumName(t.blend, kTexBlends));
	v.set("opacity", t.opacity);
	v.set("speed", t.speed);
	v.set("startTime", t.startTime);
	v.set("loop", t.loop);
	return v;
}

Texture TextureFromJson(const Value& v)
{
	Texture t;
	t.path = v["path"].asWString(L"");
	t.area = EnumFrom(v["area"], kTexAreas, t.area);
	t.fit = EnumFrom(v["fit"], kTexFits, t.fit);
	t.scale = std::max(F(v, "scale", t.scale), 1.0f);
	t.offsetX = F(v, "offsetX", t.offsetX);
	t.offsetY = F(v, "offsetY", t.offsetY);
	t.rotation = F(v, "rotation", t.rotation);
	t.blend = EnumFrom(v["blend"], kTexBlends, t.blend);
	t.opacity = std::clamp(F(v, "opacity", t.opacity), 0.0f, 100.0f);
	t.speed = F(v, "speed", t.speed);
	t.startTime = F(v, "startTime", t.startTime);
	t.loop = v["loop"].asBool(t.loop);
	return t;
}

Value PaintToJson(const Paint& p)
{
	Value v = Value::Object();
	v.set("type", EnumName(p.type, kPaintTypes));
	switch (p.type) {
	case PaintType::Texture:
		v.set("tex", TextureToJson(p.tex));
		if (p.base) v.set("base", PaintToJson(*p.base));
		break;
	case PaintType::Solid:
		v.set("color", ColorToString(p.color));
		break;
	case PaintType::Linear:
	case PaintType::Radial: {
		if (p.type == PaintType::Linear) v.set("angle", p.angle);
		Value stops = Value::Array();
		for (const auto& s : p.stops) {
			Value sv = Value::Object();
			sv.set("pos", s.pos);
			sv.set("color", ColorToString(s.color));
			stops.push(sv);
		}
		v.set("stops", stops);
		break;
	}
	case PaintType::Stripe:
	case PaintType::Dots:
		v.set("color", ColorToString(p.color));
		v.set("color2", ColorToString(p.color2));
		v.set("angle", p.angle);
		v.set("size", p.size);
		v.set("gap", p.gap);
		break;
	}
	return v;
}

Paint PaintFromJson(const Value& v)
{
	Paint p;
	p.type = EnumFrom(v["type"], kPaintTypes, PaintType::Solid);
	p.color = ColorFromString(v["color"].asString(), p.color);
	p.color2 = ColorFromString(v["color2"].asString(), p.color2);
	p.angle = F(v, "angle", p.angle);
	p.size = F(v, "size", p.size);
	p.gap = F(v, "gap", p.gap);
	for (const auto& s : v["stops"].items())
		p.stops.push_back({ F(s, "pos", 0), ColorFromString(s["color"].asString(), 0xFFFFFFFF) });
	if ((p.type == PaintType::Linear || p.type == PaintType::Radial) && p.stops.size() < 2)
		p.stops = { { 0, 0xFFFFFFFF }, { 1, 0xFF000000 } };
	if (p.type == PaintType::Texture) {
		p.tex = TextureFromJson(v["tex"]);
		// The base is any other paint; a texture on a texture would only nest.
		if (v.has("base") && v["base"]["type"].asString() != "texture") p.base = std::make_shared<const Paint>(PaintFromJson(v["base"]));
	}
	return p;
}

Value LayerToJson(const Layer& l)
{
	Value v = Value::Object();
	v.set("id", l.id);
	v.set("type", EnumName(l.type, kLayerTypes));
	v.set("name", l.name);
	v.set("enabled", l.enabled);
	v.set("opacity", l.opacity);
	if ((l.type == LayerType::Fill || l.type == LayerType::Stroke) && HasSurface(l)) {
		v.set("material", EnumName(l.material, kMaterials));
		v.set("metallic", l.metallic);
		v.set("roughness", l.roughness);
		v.set("brushed", l.brushed);
		v.set("bevel", EnumName(l.bevel, kBevels));
		v.set("bevelWidth", l.bevelWidth);
		v.set("bevelHeight", l.bevelHeight);
		if (!l.bump.path.empty()) {
			v.set("bump", TextureToJson(l.bump));
			v.set("bumpDepth", l.bumpDepth);
		}
	}
	switch (l.type) {
	case LayerType::Texture:
		v.set("tex", TextureToJson(l.tex));
		v.set("clip", l.clip);
		break;
	case LayerType::Fill:
		v.set("paint", PaintToJson(l.paint));
		break;
	case LayerType::Stroke:
		v.set("paint", PaintToJson(l.paint));
		v.set("width", l.width);
		v.set("position", EnumName(l.position, kStrokePositions));
		v.set("join", EnumName(l.join, kJoins));
		v.set("miterLimit", l.miterLimit);
		break;
	case LayerType::Shadow:
	case LayerType::InnerShadow:
		v.set("color", ColorToString(l.color));
		v.set("angle", l.angle);
		v.set("distance", l.distance);
		v.set("blur", l.blur);
		v.set("spread", l.spread);
		break;
	case LayerType::LongShadow:
		v.set("paint", PaintToJson(l.paint));
		v.set("angle", l.angle);
		v.set("length", l.length);
		break;
	case LayerType::Background:
		v.set("paint", PaintToJson(l.paint));
		v.set("padX", l.padX);
		v.set("padY", l.padY);
		v.set("radius", l.radius);
		break;
	case LayerType::Extrude:
		v.set("depth", l.depth);
		v.set("sideColor", ColorToString(l.sideColor));
		v.set("sideColor2", ColorToString(l.sideColor2));
		v.set("bevel", EnumName(l.bevel, kBevels));
		v.set("bevelWidth", l.bevelWidth);
		v.set("bevelHeight", l.bevelHeight);
		v.set("rotX", l.rotX);
		v.set("rotY", l.rotY);
		v.set("perspective", l.perspective);
		v.set("metallic", l.metallic);
		v.set("roughness", l.roughness);
		if (l.turn != TurnMode::None) {
			v.set("turn", EnumName(l.turn, kTurnModes));
			v.set("turnX", l.turnX);
			v.set("turnSpeed", l.turnSpeed);
			v.set("turnAmount", l.turnAmount);
			v.set("turnPeriod", l.turnPeriod);
			v.set("turnPhase", l.turnPhase);
		}
		break;
	}
	return v;
}

Layer LayerFromJson(const Value& v)
{
	Layer l = MakeLayer(EnumFrom(v["type"], kLayerTypes, LayerType::Fill));
	if (v["id"].isString() && !v["id"].asString().empty()) l.id = v["id"].asString();
	if (v["name"].isString()) l.name = v["name"].asWString();
	l.enabled = v["enabled"].asBool(true);
	l.opacity = F(v, "opacity", 100);
	if (v.has("paint")) l.paint = PaintFromJson(v["paint"]);
	l.width = F(v, "width", l.width);
	l.position = EnumFrom(v["position"], kStrokePositions, l.position);
	l.join = EnumFrom(v["join"], kJoins, l.join);
	l.miterLimit = F(v, "miterLimit", l.miterLimit);
	l.color = ColorFromString(v["color"].asString(), l.color);
	l.angle = F(v, "angle", l.angle);
	l.distance = F(v, "distance", l.distance);
	l.blur = F(v, "blur", l.blur);
	l.spread = F(v, "spread", l.spread);
	l.length = F(v, "length", l.length);
	l.padX = F(v, "padX", l.padX);
	l.padY = F(v, "padY", l.padY);
	l.radius = F(v, "radius", l.radius);
	l.depth = F(v, "depth", l.depth);
	l.sideColor = ColorFromString(v["sideColor"].asString(), l.sideColor);
	l.sideColor2 = ColorFromString(v["sideColor2"].asString(), l.sideColor2);
	l.bevel = EnumFrom(v["bevel"], kBevels, l.bevel);
	l.bevelWidth = F(v, "bevelWidth", l.bevelWidth);
	l.bevelHeight = F(v, "bevelHeight", l.bevelHeight);
	l.rotX = F(v, "rotX", l.rotX);
	l.rotY = F(v, "rotY", l.rotY);
	l.perspective = F(v, "perspective", l.perspective);
	l.material = EnumFrom(v["material"], kMaterials, l.material);
	l.metallic = F(v, "metallic", l.metallic);
	l.roughness = F(v, "roughness", l.roughness);
	l.brushed = v["brushed"].asBool(l.brushed);
	if (v.has("bump")) l.bump = TextureFromJson(v["bump"]);
	l.bumpDepth = F(v, "bumpDepth", l.bumpDepth);
	if (v.has("tex")) l.tex = TextureFromJson(v["tex"]);
	l.clip = v["clip"].asBool(l.clip);
	l.turn = EnumFrom(v["turn"], kTurnModes, l.turn);
	l.turnX = v["turnX"].asBool(l.turnX);
	l.turnSpeed = F(v, "turnSpeed", l.turnSpeed);
	l.turnAmount = F(v, "turnAmount", l.turnAmount);
	l.turnPeriod = std::max(F(v, "turnPeriod", l.turnPeriod), 0.05f);
	l.turnPhase = F(v, "turnPhase", l.turnPhase);
	return l;
}

namespace {

Value AnimToJson(const AnimSpec& a)
{
	Value v = Value::Object();
	v.set("type", EnumName(a.type, kAnimTypes));
	if (a.type == AnimType::None) return v;
	v.set("unit", EnumName(a.unit, kAnimUnits));
	v.set("order", EnumName(a.order, kAnimOrders));
	v.set("ease", EnumName(a.ease, kAnimEases));
	v.set("dir", EnumName(a.dir, kAnimDirs));
	v.set("duration", a.duration);
	v.set("stagger", a.stagger);
	v.set("delay", a.delay);
	v.set("amount", a.amount);
	v.set("fade", a.fade);
	v.set("seed", a.seed);
	return v;
}

AnimSpec AnimFromJson(const Value& v)
{
	AnimSpec a;
	a.type = EnumFrom(v["type"], kAnimTypes, AnimType::None);
	a.unit = EnumFrom(v["unit"], kAnimUnits, a.unit);
	a.order = EnumFrom(v["order"], kAnimOrders, a.order);
	a.ease = EnumFrom(v["ease"], kAnimEases, a.ease);
	a.dir = EnumFrom(v["dir"], kAnimDirs, a.dir);
	a.duration = F(v, "duration", a.duration);
	a.stagger = F(v, "stagger", a.stagger);
	a.delay = F(v, "delay", a.delay);
	a.amount = F(v, "amount", a.amount);
	a.fade = v["fade"].asBool(a.fade);
	a.seed = v["seed"].asInt(a.seed);
	return a;
}

Value TextObjectToJson(const TextObject& t)
{
	Value v = Value::Object();
	v.set("type", "text");
	v.set("id", t.id);
	v.set("name", t.name);
	v.set("visible", t.visible);
	v.set("text", t.text);
	v.set("x", t.x);
	v.set("y", t.y);
	v.set("align", EnumName(t.align, kHAligns));
	v.set("valign", EnumName(t.valign, kVAligns));
	v.set("rotation", t.rotation);
	v.set("scaleX", t.scaleX);
	v.set("scaleY", t.scaleY);
	v.set("opacity", t.opacity);
	v.set("family", t.family);
	v.set("size", t.size);
	v.set("weight", t.weight);
	v.set("italic", t.italic);
	v.set("tracking", t.tracking);
	v.set("lineHeight", t.lineHeight);
	v.set("vertical", t.vertical);
	v.set("tcyDigits", t.tcyDigits);
	v.set("tcyChars", t.tcyChars);
	v.set("paintPerLine", t.paintPerLine);
	v.set("rubyScale", t.rubyScale);
	v.set("rubyGap", t.rubyGap);

	Value spans = Value::Array();
	for (const Span& s : t.spans) {
		Value sv = Value::Object();
		sv.set("start", s.start);
		sv.set("length", s.length);
		if (!s.family.empty()) sv.set("family", s.family);
		if (s.size > 0) sv.set("size", s.size);
		if (s.weight > 0) sv.set("weight", s.weight);
		if (s.tracking != 0) sv.set("tracking", s.tracking);
		if (s.baselineShift != 0) sv.set("baselineShift", s.baselineShift);
		if (s.tcy) sv.set("tcy", true);
		if (!s.ruby.empty()) sv.set("ruby", s.ruby);
		if (!s.paints.empty()) {
			Value pv = Value::Object();
			for (const auto& o : s.paints) pv.set(o.layerId.c_str(), PaintToJson(o.paint));
			sv.set("paints", pv);
		}
		spans.push(sv);
	}
	v.set("spans", spans);

	Value kern = Value::Array();
	for (const auto& k : t.kerning) {
		Value kv = Value::Object();
		kv.set("index", k.first);
		kv.set("value", k.second);
		kern.push(kv);
	}
	v.set("kerning", kern);
	v.set("kernMode", EnumName(t.kernMode, kKernModes));
	v.set("opticalKern", t.opticalKern);

	Value stack = Value::Array();
	for (const Layer& l : t.stack) stack.push(LayerToJson(l));
	v.set("stack", stack);
	Value light = Value::Object();
	light.set("angle", t.light.angle);
	light.set("height", t.light.height);
	light.set("intensity", t.light.intensity);
	light.set("ambient", t.light.ambient);
	light.set("env", EnumName(t.light.env, kEnvStyles));
	if (t.light.motion != LightMotion::None) {
		light.set("motion", EnumName(t.light.motion, kLightMotions));
		light.set("range", t.light.range);
		light.set("period", t.light.period);
		light.set("start", t.light.start);
		light.set("duration", t.light.duration);
	}
	v.set("light", light);
	v.set("animIn", AnimToJson(t.animIn));
	v.set("animOut", AnimToJson(t.animOut));
	return v;
}

TextObject TextObjectFromJson(const Value& v)
{
	TextObject t;
	t.id = v["id"].isString() ? v["id"].asString() : NewId("T");
	t.name = v["name"].asWString(L"テキスト");
	t.visible = v["visible"].asBool(true);
	t.text = v["text"].asWString(L"");
	t.x = F(v, "x", t.x);
	t.y = F(v, "y", t.y);
	t.align = EnumFrom(v["align"], kHAligns, t.align);
	t.valign = EnumFrom(v["valign"], kVAligns, t.valign);
	t.rotation = F(v, "rotation", 0);
	t.scaleX = F(v, "scaleX", 100);
	t.scaleY = F(v, "scaleY", 100);
	t.opacity = F(v, "opacity", 100);
	t.family = v["family"].asWString(t.family);
	t.size = F(v, "size", t.size);
	t.weight = v["weight"].asInt(t.weight);
	t.italic = v["italic"].asBool(false);
	t.tracking = F(v, "tracking", 0);
	t.lineHeight = F(v, "lineHeight", 0);
	t.vertical = v["vertical"].asBool(false);
	t.tcyDigits = std::clamp(v["tcyDigits"].asInt(2), 0, 4);
	t.tcyChars = v["tcyChars"].asWString(L"()");
	t.paintPerLine = v["paintPerLine"].asBool(true);
	t.rubyScale = std::clamp(F(v, "rubyScale", 50), 10.0f, 200.0f);
	t.rubyGap = F(v, "rubyGap", 0);
	for (const auto& sv : v["spans"].items()) {
		Span s;
		s.start = sv["start"].asInt(0);
		s.length = sv["length"].asInt(0);
		s.family = sv["family"].asWString(L"");
		s.size = F(sv, "size", 0);
		s.weight = sv["weight"].asInt(0);
		s.tracking = F(sv, "tracking", 0);
		s.baselineShift = F(sv, "baselineShift", 0);
		s.tcy = sv["tcy"].asBool(false);
		s.ruby = sv["ruby"].asWString(L"");
		for (const auto& m : sv["paints"].members()) s.paints.push_back({ m.first, PaintFromJson(m.second) });
		if (s.length > 0) t.spans.push_back(s);
	}
	for (const auto& kv : v["kerning"].items())
		t.kerning.push_back({ kv["index"].asInt(0), F(kv, "value", 0) });
	t.kernMode = EnumFrom(v["kernMode"], kKernModes, t.kernMode);
	t.opticalKern = std::clamp(F(v, "opticalKern", 0), 0.0f, 200.0f);
	for (const auto& lv : v["stack"].items()) t.stack.push_back(LayerFromJson(lv));
	// The first Phase 3 files kept the light in the 3D layer.
	Value light = v["light"];
	if (!v.has("light"))
		for (const auto& lv : v["stack"].items())
			if (lv["type"].asString() == "extrude" && lv.has("lightAngle")) {
				light = Value::Object();
				light.set("angle", F(lv, "lightAngle", t.light.angle));
				light.set("height", F(lv, "lightHeight", t.light.height));
				light.set("intensity", F(lv, "lightIntensity", t.light.intensity));
				light.set("ambient", F(lv, "ambient", t.light.ambient));
				break;
			}
	t.light.angle = F(light, "angle", t.light.angle);
	t.light.height = F(light, "height", t.light.height);
	t.light.intensity = F(light, "intensity", t.light.intensity);
	t.light.ambient = F(light, "ambient", t.light.ambient);
	t.light.env = EnumFrom(light["env"], kEnvStyles, t.light.env);
	t.light.motion = EnumFrom(light["motion"], kLightMotions, t.light.motion);
	t.light.range = F(light, "range", t.light.range);
	t.light.period = std::max(F(light, "period", t.light.period), 0.05f);
	t.light.start = F(light, "start", t.light.start);
	t.light.duration = F(light, "duration", t.light.duration);
	if (v.has("animIn")) t.animIn = AnimFromJson(v["animIn"]);
	else t.animIn = FadeInAnim(v["fadeInFrames"].asInt(0)); // early Phase 1 files
	t.animOut = AnimFromJson(v["animOut"]);
	return t;
}

} // namespace

Value DocToJson(const Doc& d)
{
	Value v = Value::Object();
	v.set("format", "NAKA-TDP");
	v.set("version", 2);
	Value frame = Value::Object();
	frame.set("width", d.width);
	frame.set("height", d.height);
	frame.set("vidScale", d.vidScale);
	frame.set("vidSampleSize", d.vidSampleSize);
	frame.set("durationFrames", d.durationFrames);
	v.set("frame", frame);
	Value objs = Value::Array();
	for (const auto& o : d.objects) objs.push(TextObjectToJson(o));
	v.set("objects", objs);
	return v;
}

bool DocFromJson(const Value& v, Doc& out)
{
	if (v["format"].asString() != "NAKA-TDP") return false;
	Doc d;
	const Value& f = v["frame"];
	d.width = f["width"].asInt(d.width);
	d.height = f["height"].asInt(d.height);
	d.vidScale = f["vidScale"].asInt(d.vidScale);
	d.vidSampleSize = f["vidSampleSize"].asInt(d.vidSampleSize);
	d.durationFrames = f["durationFrames"].asInt(d.durationFrames);
	if (d.width <= 0 || d.height <= 0 || d.vidScale <= 0 || d.vidSampleSize <= 0) return false;
	if (d.durationFrames < 1) d.durationFrames = 1;
	for (const auto& ov : v["objects"].items())
		if (ov["type"].asString() == "text") d.objects.push_back(TextObjectFromJson(ov));
	out = std::move(d);
	return true;
}

// ---------------------------------------------------------------- files
bool ReadFileUtf8(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	GetFileSizeEx(h, &size);
	std::string bytes((size_t)size.QuadPart, '\0');
	DWORD read = 0;
	BOOL ok = bytes.empty() || ReadFile(h, bytes.data(), (DWORD)bytes.size(), &read, nullptr);
	CloseHandle(h);
	if (!ok) return false;
	bytes.resize(read);
	if (bytes.rfind("\xEF\xBB\xBF", 0) == 0) bytes.erase(0, 3);
	out = std::move(bytes);
	return true;
}

bool WriteFileUtf8(const std::wstring& path, const std::string& data)
{
	// Overwrite in place (Premiere may hold the file open with share flags).
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD written = 0;
	BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr);
	CloseHandle(h);
	return ok && written == data.size();
}

bool LoadDoc(const std::wstring& path, Doc& out)
{
	std::string bytes;
	if (!ReadFileUtf8(path, bytes)) return false;
	size_t first = bytes.find_first_not_of(" \t\r\n");
	if (first != std::string::npos && bytes[first] == '{') {
		Value v;
		return json::Parse(bytes, v) && DocFromJson(v, out);
	}
	return LoadV1(bytes, out);
}

bool SaveDoc(const std::wstring& path, const Doc& doc)
{
	return WriteFileUtf8(path, DocToJson(doc).dump(true));
}

} // namespace tdp
