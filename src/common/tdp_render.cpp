#include "tdp_render.h"
#include "tdp_media.h"

#include <windows.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dwrite_2.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

// Compiled at build time from tdp_3d.hlsl / tdp_surface.hlsl.
#include "tdp_3d_ps.h"      // g_ps3d
#include "tdp_3d_vs.h"      // g_vs3d
#include "tdp_surf_blur.h"  // g_psBlur
#include "tdp_surf_height.h" // g_psHeight
#include "tdp_surf_jump.h"  // g_psJump
#include "tdp_surf_seed.h"  // g_psSeed
#include "tdp_surf_shade.h" // g_psShade
#include "tdp_surf_vs.h"    // g_vsSurf

using Microsoft::WRL::ComPtr;

namespace tdp {

namespace {

constexpr float kPi = 3.14159265358979f;

D2D1_COLOR_F ToColor(uint32_t argb, float opacity = 1.0f)
{
	return D2D1::ColorF(((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f,
		(argb & 0xFF) / 255.0f, ((argb >> 24) & 0xFF) / 255.0f * opacity);
}

D2D1_VECTOR_4F ToVector(uint32_t argb)
{
	D2D1_COLOR_F c = ToColor(argb);
	return D2D1::Vector4F(c.r, c.g, c.b, c.a);
}

D2D1_RECT_F Inflate(const D2D1_RECT_F& r, float dx, float dy)
{
	return D2D1::RectF(r.left - dx, r.top - dy, r.right + dx, r.bottom + dy);
}

// ---------------------------------------------------------------- span tag (DirectWrite drawing effect)
struct __declspec(uuid("6b0a4f5e-3f0e-4d0e-9a51-7c2b1d6e8a01")) SpanTag : public IUnknown {
	explicit SpanTag(int i) : index(i) {}
	int index;
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** obj) override
	{
		if (riid == __uuidof(IUnknown) || riid == __uuidof(SpanTag)) { *obj = this; AddRef(); return S_OK; }
		*obj = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
	ULONG STDMETHODCALLTYPE Release() override { ULONG r = --ref; if (!r) delete this; return r; }
	ULONG ref = 1;
};

// ---------------------------------------------------------------- glyph outlines
// One character (cluster) of outline.
struct Run {
	ComPtr<ID2D1Geometry> geometry; // layout space, at rest
	int span = -1;                  // index into TextObject::spans, or -1
	int ch = 0;                     // first UTF-16 index of the cluster
	D2D1_RECT_F cell{};             // advance x ascent..descent box, layout space
	D2D1_MATRIX_3X2_F m = D2D1::IdentityMatrix(); // animation transform (layout space)
	float opacity = 1;              // animation opacity
	ComPtr<ID2D1Geometry> placed;   // geometry with m applied
	bool ruby = false;              // ruby text over the characters starting at ch
	D2D1_RECT_F paintBox{};         // area gradients / patterns span: its line, or the whole object
	// 3D turn of the unit (degrees) around pivot (layout space, after m); drawn by the 3D layer.
	float rotX = 0, rotY = 0;
	D2D1_POINT_2F pivot{};
	float dz = 0;                   // doc px away from the viewer (fly in / out), drawn by the 3D layer
	float depth = 1;                // growth of the 3D extrusion (0 = flat)
};

// IDWriteTextRenderer1 so that vertical text arrives with its glyph orientation.
class OutlineCollector : public IDWriteTextRenderer1 {
public:
	OutlineCollector(ID2D1Factory* factory, IDWriteTextAnalyzer2* analyzer, const TextObject& obj)
		: factory_(factory), analyzer_(analyzer), obj_(obj) {}

	std::vector<Run> runs;

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** obj) override
	{
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) || riid == __uuidof(IDWriteTextRenderer)
			|| riid == __uuidof(IDWriteTextRenderer1)) {
			*obj = static_cast<IDWriteTextRenderer1*>(this);
			AddRef();
			return S_OK;
		}
		*obj = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
	ULONG STDMETHODCALLTYPE Release() override { return --ref_; } // stack object

	HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override { *disabled = TRUE; return S_OK; }
	HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override
	{
		*m = DWRITE_MATRIX{ 1, 0, 0, 1, 0, 0 };
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* ppd) override { *ppd = 1.0f; return S_OK; }

	HRESULT STDMETHODCALLTYPE DrawGlyphRun(void* ctx, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode,
		const DWRITE_GLYPH_RUN* glyphRun, const DWRITE_GLYPH_RUN_DESCRIPTION* desc, IUnknown* effect) override
	{
		return DrawGlyphRun(ctx, x, y, DWRITE_GLYPH_ORIENTATION_ANGLE_0_DEGREES, mode, glyphRun, desc, effect);
	}

	// Splits the glyph run into clusters so that every character can be animated on its own.
	// The outline is built as a horizontal run at the origin (x, y), then rotated into place with the
	// orientation transform (vertical text: the run is turned 90 degrees, upright CJK glyphs are sideways in it).
	HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_GLYPH_ORIENTATION_ANGLE angle, DWRITE_MEASURING_MODE,
		const DWRITE_GLYPH_RUN* glyphRun, const DWRITE_GLYPH_RUN_DESCRIPTION* desc, IUnknown* effect) override
	{
		int span = -1;
		ComPtr<SpanTag> tag;
		if (effect && SUCCEEDED(effect->QueryInterface(__uuidof(SpanTag), reinterpret_cast<void**>(tag.GetAddressOf()))))
			span = tag->index;

		D2D1::Matrix3x2F orient = D2D1::Matrix3x2F::Identity();
		if (analyzer_ && (angle != DWRITE_GLYPH_ORIENTATION_ANGLE_0_DEGREES || glyphRun->isSideways)) {
			DWRITE_MATRIX m{};
			if (SUCCEEDED(analyzer_->GetGlyphOrientationTransform(angle, glyphRun->isSideways, x, y, &m)))
				orient = D2D1::Matrix3x2F(m.m11, m.m12, m.m21, m.m22, m.dx, m.dy);
		}
		// Baseline shift is "up" in the run's own frame (= to the right in vertical text).
		const float shift = span >= 0 && span < (int)obj_.spans.size() ? obj_.spans[span].baselineShift : 0.0f;
		y -= shift;

		const UINT32 n = glyphRun->glyphCount;
		if (n == 0) return S_OK;
		std::vector<int> glyphChar(n, -1);
		const int base = desc ? (int)desc->textPosition : 0;
		if (desc && desc->clusterMap)
			for (UINT32 i = 0; i < desc->stringLength; ++i) {
				const UINT16 g = desc->clusterMap[i];
				if (g < n && glyphChar[g] < 0) glyphChar[g] = base + (int)i;
			}
		for (UINT32 g = 0; g < n; ++g)
			if (glyphChar[g] < 0) glyphChar[g] = g ? glyphChar[g - 1] : base;

		DWRITE_FONT_METRICS fm{};
		glyphRun->fontFace->GetMetrics(&fm);
		const float k = glyphRun->fontEmSize / std::max<float>(fm.designUnitsPerEm, 1.0f);
		const float ascent = fm.ascent * k, descent = fm.descent * k;
		const bool rtl = glyphRun->bidiLevel % 2 != 0;

		float pen = x;
		for (UINT32 g0 = 0; g0 < n;) {
			UINT32 g1 = g0 + 1;
			while (g1 < n && glyphChar[g1] == glyphChar[g0]) ++g1;
			float advance = 0;
			for (UINT32 g = g0; g < g1; ++g) advance += glyphRun->glyphAdvances[g];
			const float left = rtl ? pen - advance : pen;

			ComPtr<ID2D1PathGeometry> path;
			HRESULT hr = factory_->CreatePathGeometry(&path);
			if (FAILED(hr)) return hr;
			ComPtr<ID2D1GeometrySink> sink;
			hr = path->Open(&sink);
			if (FAILED(hr)) return hr;
			sink->SetFillMode(D2D1_FILL_MODE_WINDING);
			hr = glyphRun->fontFace->GetGlyphRunOutline(glyphRun->fontEmSize, glyphRun->glyphIndices + g0, glyphRun->glyphAdvances + g0,
				glyphRun->glyphOffsets ? glyphRun->glyphOffsets + g0 : nullptr, g1 - g0, glyphRun->isSideways, rtl, sink.Get());
			if (FAILED(hr)) return hr;
			hr = sink->Close();
			if (FAILED(hr)) return hr;
			ComPtr<ID2D1TransformedGeometry> moved;
			hr = factory_->CreateTransformedGeometry(path.Get(), D2D1::Matrix3x2F::Translation(pen, y) * orient, &moved);
			if (FAILED(hr)) return hr;

			Run r;
			r.geometry = moved;
			r.placed = moved;
			r.span = span;
			r.ch = glyphChar[g0];
			// Character cell (advance x ascent..descent) in layout space: bounds of the rotated box.
			const D2D1_POINT_2F corners[4] = { { left, y - ascent }, { left + advance, y - ascent },
				{ left + advance, y + descent }, { left, y + descent } };
			r.cell = D2D1::RectF(1e9f, 1e9f, -1e9f, -1e9f);
			for (const auto& c : corners) {
				const D2D1_POINT_2F p = orient.TransformPoint(c);
				r.cell = D2D1::RectF(std::min(r.cell.left, p.x), std::min(r.cell.top, p.y), std::max(r.cell.right, p.x), std::max(r.cell.bottom, p.y));
			}
			runs.push_back(std::move(r));
			pen += rtl ? -advance : advance;
			g0 = g1;
		}
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, DWRITE_GLYPH_ORIENTATION_ANGLE, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, DWRITE_GLYPH_ORIENTATION_ANGLE, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, DWRITE_GLYPH_ORIENTATION_ANGLE, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }

private:
	ID2D1Factory* factory_;
	IDWriteTextAnalyzer2* analyzer_;
	const TextObject& obj_;
	ULONG ref_ = 1;
};

// Everything needed to draw one text object, in layout space.
struct TextShape {
	std::vector<Run> runs;
	ComPtr<ID2D1Geometry> all;   // union of every run, as placed
	D2D1_RECT_F ink{};           // geometry bounds at rest (layout space): gradients, patterns, background
	bool background = true;      // draw background layers (only one opacity group does)
	D2D1_MATRIX_3X2_F bgM = D2D1::IdentityMatrix(); // whole-object ("all") animation, applied to the background
	D2D1_RECT_F box{};           // layout box (layout space)
	D2D1_MATRIX_3X2_F toOutput{}; // layout space -> output pixels
	float deviceScale = 1;       // doc px -> output px, including the object's scale
	D2D1_POINT_2F anchorOut{};   // the object's anchor in output pixels (3D turns pivot here)
};

// ---------------------------------------------------------------- 3D mesh
// Vertex of the 3D layer (layout space while built, output pixels when drawn). Matches tdp_3d.hlsl.
struct Vertex3D {
	float x, y, z;
	float nx, ny, nz;
	float u, v;
	float kind, t; // 0 front, 1 bevel, 2 side, 3 back; side gradient 0 (front) .. 1 (back)
};

using Poly = std::vector<D2D1_POINT_2F>;

// Collects the figures of a geometry flattened to lines.
class PolySink : public ID2D1SimplifiedGeometrySink {
public:
	std::vector<Poly> figures;
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** obj) override
	{
		if (riid == __uuidof(IUnknown) || riid == __uuidof(ID2D1SimplifiedGeometrySink)) { *obj = this; return S_OK; }
		*obj = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return 1; } // stack object
	ULONG STDMETHODCALLTYPE Release() override { return 1; }
	void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE) override {}
	void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT) override {}
	void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F p, D2D1_FIGURE_BEGIN) override { figures.push_back({ p }); }
	void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F* p, UINT32 n) override { figures.back().insert(figures.back().end(), p, p + n); }
	void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT* b, UINT32 n) override
	{
		for (UINT32 i = 0; i < n; ++i) figures.back().push_back(b[i].point3);
	}
	void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END) override {}
	HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }
};

class TriSink : public ID2D1TessellationSink {
public:
	std::vector<D2D1_TRIANGLE> triangles;
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** obj) override
	{
		if (riid == __uuidof(IUnknown) || riid == __uuidof(ID2D1TessellationSink)) { *obj = this; return S_OK; }
		*obj = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return 1; } // stack object
	ULONG STDMETHODCALLTYPE Release() override { return 1; }
	void STDMETHODCALLTYPE AddTriangles(const D2D1_TRIANGLE* t, UINT32 n) override { triangles.insert(triangles.end(), t, t + n); }
	HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }
};

float SignedArea(const Poly& p)
{
	double a = 0;
	for (size_t i = 0, n = p.size(); i < n; ++i) {
		const D2D1_POINT_2F& q = p[i], & r = p[(i + 1) % n];
		a += (double)q.x * r.y - (double)r.x * q.y;
	}
	return (float)(a * 0.5);
}

// One step of the bevel cross-section, from the outer edge (first) to the front face (last).
struct BevelStep {
	float d, z;     // inset from the outline, depth from the front
	float no, nf;   // surface normal: outward part, towards-the-viewer part
};

std::vector<BevelStep> BevelProfile(const Layer& l, float depth, float& width, float& height)
{
	width = std::max(l.bevelWidth, 0.0f);
	height = std::clamp(l.bevelHeight, 0.0f, depth);
	if (l.bevel == BevelShape::None || width <= 0 || height <= 0) { width = height = 0; return {}; }
	const int steps = l.bevel == BevelShape::Flat ? 1 : 6;
	std::vector<BevelStep> out;
	for (int k = 0; k <= steps; ++k) {
		const float t = (float)k / steps, th = t * kPi * 0.5f;
		float d, z, dd, dz; // position and its derivative along the cross-section
		if (l.bevel == BevelShape::Flat) {
			d = width * t; z = height * (1 - t); dd = width; dz = -height;
		} else if (l.bevel == BevelShape::Cove) {
			d = width * std::sin(th); z = height * std::cos(th); dd = width * std::cos(th); dz = -height * std::sin(th);
		} else { // round (convex quarter circle)
			d = width * (1 - std::cos(th)); z = height * (1 - std::sin(th)); dd = width * std::sin(th); dz = -height * std::cos(th);
		}
		const float len = std::max(std::hypot(dz, dd), 1e-6f);
		out.push_back({ d, z, -dz / len, dd / len });
	}
	return out;
}

// Extrudes the figures (clean outline, layout space) to `depth` with the bevel profile.
// Sides and bevel are built here; the outlines of the front (inset) and back faces are returned
// for tessellation.
void ExtrudeFigures(const std::vector<Poly>& figs, const std::vector<BevelStep>& prof, float bevelH, float depth,
	std::vector<Vertex3D>& out, std::vector<Poly>& frontOutline)
{
	// Outer outlines and holes run in opposite directions: the largest figure tells which side is ink.
	float big = 0;
	for (const Poly& p : figs) { const float a = SignedArea(p); if (std::fabs(a) > std::fabs(big)) big = a; }
	const float sgn = big >= 0 ? 1.0f : -1.0f;
	const float smoothCos = 0.77f; // corners sharper than ~40 degrees keep a hard edge
	const float frontInset = prof.empty() ? 0.0f : prof.back().d;

	for (const Poly& p : figs) {
		const size_t n = p.size();
		if (n < 3) continue;
		std::vector<D2D1_POINT_2F> o(n), avg(n), miter(n);
		std::vector<bool> smooth(n);
		for (size_t i = 0; i < n; ++i) {
			const D2D1_POINT_2F& a = p[i], & b = p[(i + 1) % n];
			const float ex = b.x - a.x, ey = b.y - a.y, len = std::hypot(ex, ey);
			o[i] = len > 1e-6f ? D2D1::Point2F(sgn * ey / len, -sgn * ex / len) : (i ? o[i - 1] : D2D1::Point2F(0, 0)); // outward
		}
		for (size_t i = 0; i < n; ++i) {
			const D2D1_POINT_2F a = o[(i + n - 1) % n], b = o[i];
			const float c = a.x * b.x + a.y * b.y;
			smooth[i] = c > smoothCos;
			const float sx = a.x + b.x, sy = a.y + b.y, sl = std::hypot(sx, sy);
			avg[i] = sl > 1e-6f ? D2D1::Point2F(sx / sl, sy / sl) : b;
			// Inward offset that moves both neighbouring edges by the same distance.
			const float k = 1.0f / std::max(1.0f + c, 0.25f);
			miter[i] = D2D1::Point2F(-sx * k, -sy * k);
		}
		auto at = [&](size_t i, float d) { return D2D1::Point2F(p[i].x + miter[i].x * d, p[i].y + miter[i].y * d); };
		auto vtx = [](D2D1_POINT_2F q, float z, D2D1_POINT_2F nrm, float no, float nf, float kind, float t) {
			return Vertex3D{ q.x, q.y, z, nrm.x * no, nrm.y * no, -nf, 0, 0, kind, t };
		};
		auto quad = [&](const Vertex3D& a, const Vertex3D& b, const Vertex3D& c, const Vertex3D& d) {
			out.push_back(a); out.push_back(b); out.push_back(c);
			out.push_back(a); out.push_back(c); out.push_back(d);
		};
		for (size_t i = 0; i < n; ++i) {
			const size_t j = (i + 1) % n;
			const D2D1_POINT_2F na = smooth[i] ? avg[i] : o[i], nb = smooth[j] ? avg[j] : o[i];
			for (size_t k = 0; k + 1 < prof.size(); ++k) {
				const BevelStep& s0 = prof[k], & s1 = prof[k + 1];
				quad(vtx(at(i, s0.d), s0.z, na, s0.no, s0.nf, 1, 0), vtx(at(j, s0.d), s0.z, nb, s0.no, s0.nf, 1, 0),
					vtx(at(j, s1.d), s1.z, nb, s1.no, s1.nf, 1, 0), vtx(at(i, s1.d), s1.z, na, s1.no, s1.nf, 1, 0));
			}
			if (depth > bevelH) {
				const float t0 = depth > 0 ? bevelH / depth : 0;
				quad(vtx(p[i], bevelH, na, 1, 0, 2, t0), vtx(p[j], bevelH, nb, 1, 0, 2, t0),
					vtx(p[j], depth, nb, 1, 0, 2, 1), vtx(p[i], depth, na, 1, 0, 2, 1));
			}
		}
		Poly inset(n);
		for (size_t i = 0; i < n; ++i) inset[i] = at(i, frontInset);
		frontOutline.push_back(std::move(inset));
	}
}

// ---------------------------------------------------------------- 3x3 matrices for the 3D layer
struct M3 {
	float m[3][3];
	static M3 Identity() { return { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } }; }
	// x right, y down, z away from the viewer. Positive Y turns the right side away, positive X the bottom.
	static M3 RotY(float deg)
	{
		const float a = deg * kPi / 180.0f, c = std::cos(a), s = std::sin(a);
		return { { { c, 0, -s }, { 0, 1, 0 }, { s, 0, c } } };
	}
	static M3 RotX(float deg)
	{
		const float a = deg * kPi / 180.0f, c = std::cos(a), s = std::sin(a);
		return { { { 1, 0, 0 }, { 0, c, -s }, { 0, s, c } } };
	}
	// The 2x2 part of a D2D matrix, z scaled by zs.
	static M3 From2D(const D2D1_MATRIX_3X2_F& t, float zs) { return { { { t._11, t._21, 0 }, { t._12, t._22, 0 }, { 0, 0, zs } } }; }
	M3 operator*(const M3& b) const
	{
		M3 r{};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				r.m[i][j] = m[i][0] * b.m[0][j] + m[i][1] * b.m[1][j] + m[i][2] * b.m[2][j];
		return r;
	}
	void Apply(float& x, float& y, float& z) const
	{
		const float a = m[0][0] * x + m[0][1] * y + m[0][2] * z, b = m[1][0] * x + m[1][1] * y + m[1][2] * z;
		z = m[2][0] * x + m[2][1] * y + m[2][2] * z;
		x = a;
		y = b;
	}
	// Transforms normals (cofactor matrix = inverse transpose up to scale).
	M3 NormalMatrix() const
	{
		auto c = [&](int r0, int r1, int c0, int c1) { return m[r0][c0] * m[r1][c1] - m[r0][c1] * m[r1][c0]; };
		M3 r = { { { c(1, 2, 1, 2), -c(1, 2, 0, 2), c(1, 2, 0, 1) },
			{ -c(0, 2, 1, 2), c(0, 2, 0, 2), -c(0, 2, 0, 1) },
			{ c(0, 1, 1, 2), -c(0, 1, 0, 2), c(0, 1, 0, 1) } } };
		const float det = m[0][0] * r.m[0][0] + m[0][1] * r.m[0][1] + m[0][2] * r.m[0][2];
		if (det < 0)
			for (auto& row : r.m) for (float& v : row) v = -v;
		return r;
	}
};

struct float4v {
	float v[4];
};

// Direction towards the light (world space) and its intensity.
float4v LightVector(const Light& l)
{
	const float az = l.angle * kPi / 180.0f, el = std::clamp(l.height, -89.0f, 90.0f) * kPi / 180.0f;
	return { { std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), -std::sin(el), std::max(l.intensity, 0.0f) / 100.0f } };
}

// ---------------------------------------------------------------- engine
class Engine {
public:
	static Engine& Get()
	{
		static Engine e;
		return e;
	}

	bool Render(const Doc& doc, int frame, int outW, int outH, uint8_t* dst, int dstRowBytes, bool bottomUp)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (int attempt = 0; attempt < 2; ++attempt) {
			if (!dc_ && !Init()) return false;
			HRESULT hr = RenderLocked(doc, frame, outW, outH, dst, dstRowBytes, bottomUp);
			if (SUCCEEDED(hr)) return true;
			// Device lost etc.: rebuild everything once.
			Reset();
		}
		return false;
	}

	bool Quad(const Doc& doc, size_t index, float out[8])
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!dc_ && !Init()) return false;
		if (index >= doc.objects.size()) return false;
		TextShape shape;
		if (FAILED(BuildShape(doc.objects[index], 1.0f, shape))) return false;
		const D2D1_POINT_2F corners[4] = {
			{ shape.box.left, shape.box.top }, { shape.box.right, shape.box.top },
			{ shape.box.right, shape.box.bottom }, { shape.box.left, shape.box.bottom } };
		const D2D1::Matrix3x2F m = *D2D1::Matrix3x2F::ReinterpretBaseType(&shape.toOutput);
		for (int i = 0; i < 4; ++i) {
			D2D1_POINT_2F p = m.TransformPoint(corners[i]);
			out[i * 2] = p.x;
			out[i * 2 + 1] = p.y;
		}
		return true;
	}

	bool Cells(const Doc& doc, size_t index, std::vector<CharCell>& out)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!dc_ && !Init()) return false;
		if (index >= doc.objects.size()) return false;
		TextShape shape;
		if (FAILED(BuildShape(doc.objects[index], 1.0f, shape))) return false;
		const D2D1::Matrix3x2F m = *D2D1::Matrix3x2F::ReinterpretBaseType(&shape.toOutput);
		out.clear();
		for (const Run& r : shape.runs) {
			if (r.ruby) continue;
			CharCell c;
			c.ch = r.ch;
			const D2D1_POINT_2F corners[4] = { { r.cell.left, r.cell.top }, { r.cell.right, r.cell.top },
				{ r.cell.right, r.cell.bottom }, { r.cell.left, r.cell.bottom } };
			for (int i = 0; i < 4; ++i) {
				const D2D1_POINT_2F p = m.TransformPoint(corners[i]);
				c.quad[i * 2] = p.x;
				c.quad[i * 2 + 1] = p.y;
			}
			out.push_back(c);
		}
		std::sort(out.begin(), out.end(), [](const CharCell& a, const CharCell& b) { return a.ch < b.ch; });
		return true;
	}

	std::wstring Description()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!dc_ && !Init()) return L"";
		return description_;
	}

private:
	enum Slot { kFrame, kAccum, kTmp, kA, kB, kC, kSlotCount };

	bool Init()
	{
		const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
		const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
		HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
			D3D11_SDK_VERSION, &device_, nullptr, &context_);
		bool gpu = SUCCEEDED(hr);
		if (!gpu) {
			hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels),
				D3D11_SDK_VERSION, &device_, nullptr, &context_);
			if (FAILED(hr)) return false;
		}

		D2D1_FACTORY_OPTIONS opts{};
		hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &opts, &factory_);
		if (FAILED(hr)) return false;
		ComPtr<IDXGIDevice> dxgi;
		if (FAILED(device_.As(&dxgi))) return false;
		if (FAILED(factory_->CreateDevice(dxgi.Get(), &d2dDevice_))) return false;
		if (FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return false;
		dc_->SetDpi(96.0f, 96.0f);
		dc_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
		if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
			reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
			return false;
		ComPtr<IDWriteTextAnalyzer> analyzer;
		if (SUCCEEDED(dwrite_->CreateTextAnalyzer(&analyzer))) analyzer.As(&analyzer2_); // vertical text (Windows 8.1+)

		description_ = gpu ? L"GPU: " : L"WARP (CPU): ";
		ComPtr<IDXGIAdapter> adapter;
		DXGI_ADAPTER_DESC desc{};
		if (SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)))
			description_ += desc.Description;
		return true;
	}

	void Reset()
	{
		texCache_.clear();
		Reset3D();
		for (auto& b : slots_) b.Reset();
		for (auto& t : slotTex_) t.Reset();
		for (auto& v : slotSrv_) v.Reset();
		for (auto& v : slotRtv_) v.Reset();
		ResetSeeds();
		staging_.Reset();
		frameTexture_.Reset();
		targetW_ = targetH_ = 0;
		dc_.Reset();
		d2dDevice_.Reset();
		factory_.Reset();
		context_.Reset();
		device_.Reset();
		analyzer2_.Reset();
		dwrite_.Reset();
	}

	HRESULT EnsureTargets(int w, int h)
	{
		if (frameTexture_ && w == targetW_ && h == targetH_) return S_OK;
		for (auto& b : slots_) b.Reset();
		for (auto& t : slotTex_) t.Reset();
		for (auto& v : slotSrv_) v.Reset();
		for (auto& v : slotRtv_) v.Reset();
		ResetSeeds();
		staging_.Reset();
		frameTexture_.Reset();

		D3D11_TEXTURE2D_DESC td{};
		td.Width = (UINT)w;
		td.Height = (UINT)h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		HRESULT hr = device_->CreateTexture2D(&td, nullptr, &frameTexture_);
		if (FAILED(hr)) return hr;

		td.Usage = D3D11_USAGE_STAGING;
		td.BindFlags = 0;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		hr = device_->CreateTexture2D(&td, nullptr, &staging_);
		if (FAILED(hr)) return hr;

		ComPtr<IDXGISurface> surface;
		hr = frameTexture_.As(&surface);
		if (FAILED(hr)) return hr;
		const D2D1_PIXEL_FORMAT pf = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
		D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, pf, 96.0f, 96.0f);
		hr = dc_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &slots_[kFrame]);
		if (FAILED(hr)) return hr;

		// Scratch surfaces for the appearance stack (drawable + targetable). Own textures, so that
		// the 3D layer can read the layers in front of it (kAccum) as a shader resource.
		D2D1_BITMAP_PROPERTIES1 sp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, pf, 96.0f, 96.0f);
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		td.CPUAccessFlags = 0;
		for (int i = kAccum; i < kSlotCount; ++i) {
			hr = device_->CreateTexture2D(&td, nullptr, &slotTex_[i]);
			if (FAILED(hr)) return hr;
			ComPtr<IDXGISurface> s;
			hr = slotTex_[i].As(&s);
			if (FAILED(hr)) return hr;
			hr = dc_->CreateBitmapFromDxgiSurface(s.Get(), &sp, &slots_[i]);
			if (FAILED(hr)) return hr;
		}
		for (int i = kAccum; i < kSlotCount; ++i) {
			hr = device_->CreateShaderResourceView(slotTex_[i].Get(), nullptr, &slotSrv_[i]);
			if (FAILED(hr)) return hr;
			hr = device_->CreateRenderTargetView(slotTex_[i].Get(), nullptr, &slotRtv_[i]);
			if (FAILED(hr)) return hr;
		}
		targetW_ = w;
		targetH_ = h;
		return S_OK;
	}

	// ------------------------------------------------------------ text layout -> geometry
	// frame < 0: no animation (selection outline etc.)
	HRESULT BuildShape(const TextObject& obj, float outScale, TextShape& shape, int frame = -1, int durationFrames = 0)
	{
		// Optical kerning: the extra spacing is worked out from the outlines, then laid out like manual kerning.
		if (obj.opticalKern > 0 && obj.text.size() > 1) {
			std::vector<float> opt;
			if (SUCCEEDED(OpticalOffsets(obj, opt))) {
				TextObject adjusted = obj;
				adjusted.opticalKern = 0;
				bool any = false;
				for (size_t i = 0; i < opt.size(); ++i)
					if (std::fabs(opt[i]) > 0.01f) { adjusted.kerning.push_back({ (int)i, opt[i] }); any = true; }
				if (any) return BuildShape(adjusted, outScale, shape, frame, durationFrames);
			}
		}
		const UINT32 len = (UINT32)obj.text.size();
		ComPtr<IDWriteTextFormat> format;
		HRESULT hr = dwrite_->CreateTextFormat(obj.family.c_str(), nullptr, (DWRITE_FONT_WEIGHT)std::clamp(obj.weight, 1, 999),
			obj.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
			std::max(obj.size, 1.0f), L"ja-jp", &format);
		if (FAILED(hr)) return hr;
		format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
		format->SetTextAlignment(obj.align == HAlign::Left ? DWRITE_TEXT_ALIGNMENT_LEADING
			: obj.align == HAlign::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_CENTER);
		if (obj.lineHeight > 0)
			format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, obj.lineHeight, obj.lineHeight * 0.8f);
		if (obj.vertical) SetVertical(format.Get());

		ComPtr<IDWriteTextLayout> layout;
		hr = dwrite_->CreateTextLayout(obj.text.c_str(), len, format.Get(), 100000.0f, 100000.0f, &layout);
		if (FAILED(hr)) return hr;

		for (size_t i = 0; i < obj.spans.size(); ++i) {
			const Span& s = obj.spans[i];
			if (s.start < 0 || s.length <= 0 || (UINT32)s.start >= len) continue;
			DWRITE_TEXT_RANGE r{ (UINT32)s.start, std::min((UINT32)s.length, len - (UINT32)s.start) };
			if (!s.family.empty()) layout->SetFontFamilyName(s.family.c_str(), r);
			if (s.size > 0) layout->SetFontSize(s.size, r);
			if (s.weight > 0) layout->SetFontWeight((DWRITE_FONT_WEIGHT)std::clamp(s.weight, 1, 999), r);
			SpanTag* tag = new SpanTag((int)i);
			layout->SetDrawingEffect(tag, r);
			tag->Release();
		}

		// Font features: proportional metrics (palt / vpal) for tight Japanese, or the kerning pairs off.
		if (obj.kernMode != KernMode::Metrics && len > 0) {
			ComPtr<IDWriteTypography> typo;
			if (SUCCEEDED(dwrite_->CreateTypography(&typo))) {
				// Setting a typography replaces the default features: the kerning pairs have to be asked for again.
				const bool palt = obj.kernMode == KernMode::Palt;
				const DWRITE_FONT_FEATURE kern{ DWRITE_FONT_FEATURE_TAG_KERNING, palt ? 1u : 0u };
				typo->AddFontFeature(kern);
				if (palt) {
					const DWRITE_FONT_FEATURE prop{ obj.vertical ? (DWRITE_FONT_FEATURE_TAG)DWRITE_MAKE_OPENTYPE_TAG('v', 'p', 'a', 'l')
						: (DWRITE_FONT_FEATURE_TAG)DWRITE_MAKE_OPENTYPE_TAG('p', 'a', 'l', 't'), 1u };
					typo->AddFontFeature(prop);
				}
				layout->SetTypography(typo.Get(), { 0, len });
			}
		}

		std::vector<TcyGroup> tcy;
		hr = MeasureTcy(obj, tcy);
		if (FAILED(hr)) return hr;

		// Tracking + per-span tracking + pair kerning as trailing spacing per character.
		ComPtr<IDWriteTextLayout1> layout1;
		if (len > 0 && SUCCEEDED(layout.As(&layout1))) {
			std::vector<float> trailing(len, obj.tracking);
			for (const Span& s : obj.spans)
				for (int c = std::max(s.start, 0); c < s.start + s.length && c < (int)len; ++c) trailing[c] += s.tracking;
			// Tate-chu-yoko: the sideways characters of a group take exactly one em along the column.
			for (const TcyGroup& g : tcy) {
				for (int c = g.start; c < g.start + g.length - 1; ++c) trailing[c] = 0;
				trailing[g.start + g.length - 1] = g.em - g.width;
			}
			for (const auto& k : obj.kerning)
				if (k.first >= 0 && k.first < (int)len) trailing[k.first] += k.second;
			UINT32 start = 0;
			for (UINT32 c = 1; c <= len; ++c) {
				if (c == len || trailing[c] != trailing[start]) {
					if (trailing[start] != 0) layout1->SetCharacterSpacing(0, trailing[start], 0, { start, c - start });
					start = c;
				}
			}
		}

		// Shrink the layout box to the text so that alignment works inside the block.
		DWRITE_TEXT_METRICS tm{};
		layout->GetMetrics(&tm);
		// Metrics are physical: in vertical text the columns run along the height.
		if (obj.vertical) {
			layout->SetMaxWidth(std::max(tm.width, 1.0f));
			layout->SetMaxHeight(std::max(tm.height, 1.0f));
		} else {
			layout->SetMaxWidth(std::max(tm.widthIncludingTrailingWhitespace, 1.0f));
		}
		layout->GetMetrics(&tm);
		const float boxW = std::max(obj.vertical ? tm.width : tm.widthIncludingTrailingWhitespace, 1.0f);
		shape.box = D2D1::RectF(tm.left, tm.top, tm.left + boxW, tm.top + std::max(tm.height, 1.0f));

		OutlineCollector collector(factory_.Get(), analyzer2_.Get(), obj);
		hr = layout->Draw(nullptr, &collector, 0.0f, 0.0f);
		if (FAILED(hr)) return hr;
		shape.runs = std::move(collector.runs);
		hr = PlaceTcy(obj, tcy, shape.runs);
		if (FAILED(hr)) return hr;
		hr = PlaceRuby(obj, shape.runs);
		if (FAILED(hr)) return hr;

		if (!shape.runs.empty()) {
			hr = MakeUnion(shape);
			if (FAILED(hr)) return hr;
			shape.all->GetBounds(nullptr, &shape.ink);
			SetPaintBoxes(obj, layout.Get(), shape);
			if (frame >= 0 && HasAnimation(obj)) {
				hr = Animate(obj, layout.Get(), frame, durationFrames, shape);
				if (FAILED(hr)) return hr;
			}
		} else {
			shape.ink = shape.box;
		}

		const float cx = (shape.box.left + shape.box.right) * 0.5f, cy = (shape.box.top + shape.box.bottom) * 0.5f;
		float ax, ay;
		if (obj.vertical) {
			// align runs along the column (left = top); valign picks the side (top = first column = right).
			ay = obj.align == HAlign::Left ? shape.box.top : obj.align == HAlign::Right ? shape.box.bottom : cy;
			ax = obj.valign == VAlign::Top ? shape.box.right : obj.valign == VAlign::Bottom ? shape.box.left : cx;
		} else {
			ax = obj.align == HAlign::Left ? shape.box.left : obj.align == HAlign::Right ? shape.box.right : cx;
			ay = obj.valign == VAlign::Top ? shape.box.top : obj.valign == VAlign::Bottom ? shape.box.bottom : cy;
		}
		const float sx = obj.scaleX / 100.0f, sy = obj.scaleY / 100.0f;
		shape.toOutput = D2D1::Matrix3x2F::Translation(-ax, -ay)
			* D2D1::Matrix3x2F::Scale(sx, sy)
			* D2D1::Matrix3x2F::Rotation(obj.rotation)
			* D2D1::Matrix3x2F::Translation(obj.x, obj.y)
			* D2D1::Matrix3x2F::Scale(outScale, outScale);
		shape.deviceScale = outScale * std::sqrt(std::fabs(sx * sy));
		shape.anchorOut = D2D1::Point2F(obj.x * outScale, obj.y * outScale);
		return S_OK;
	}

	// ------------------------------------------------------------ tate-chu-yoko
	struct TcyGroup {
		int start = 0, length = 0;
		float em = 0;                       // font size of the group = cell length along the column
		float width = 0, height = 0;        // horizontal layout of the group
		ComPtr<IDWriteTextLayout> layout;   // horizontal, at the group's font
	};

	// Text format with the font of one character (object font + the spans over it), size scaled.
	HRESULT FormatAt(const TextObject& obj, int index, float scale, ComPtr<IDWriteTextFormat>& format, float& em)
	{
		std::wstring family = obj.family;
		float size = obj.size;
		int weight = obj.weight;
		for (const Span& s : obj.spans) {
			if (index < s.start || index >= s.start + s.length) continue;
			if (!s.family.empty()) family = s.family;
			if (s.size > 0) size = s.size;
			if (s.weight > 0) weight = s.weight;
		}
		em = std::max(size, 1.0f);
		return dwrite_->CreateTextFormat(family.c_str(), nullptr, (DWRITE_FONT_WEIGHT)std::clamp(weight, 1, 999),
			obj.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
			std::max(em * scale, 1.0f), L"ja-jp", &format);
	}

	static void SetVertical(IDWriteTextFormat* format)
	{
		// Columns top to bottom, laid out right to left; CJK upright, Latin sideways.
		ComPtr<IDWriteTextFormat1> format1;
		if (SUCCEEDED(format->QueryInterface(IID_PPV_ARGS(&format1))))
			format1->SetVerticalGlyphOrientation(DWRITE_VERTICAL_GLYPH_ORIENTATION_DEFAULT);
		format->SetFlowDirection(DWRITE_FLOW_DIRECTION_RIGHT_TO_LEFT);
		format->SetReadingDirection(DWRITE_READING_DIRECTION_TOP_TO_BOTTOM);
	}

	// Lays every group out horizontally with the font of its first character.
	HRESULT MeasureTcy(const TextObject& obj, std::vector<TcyGroup>& out)
	{
		for (const auto& range : TcyRanges(obj)) {
			TcyGroup g;
			g.start = range.first;
			g.length = range.second;
			ComPtr<IDWriteTextFormat> format;
			HRESULT hr = FormatAt(obj, g.start, 1.0f, format, g.em);
			if (FAILED(hr)) return hr;
			format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
			hr = dwrite_->CreateTextLayout(obj.text.c_str() + g.start, (UINT32)g.length, format.Get(), 100000.0f, 100000.0f, &g.layout);
			if (FAILED(hr)) return hr;
			DWRITE_TEXT_METRICS tm{};
			g.layout->GetMetrics(&tm);
			g.width = std::max(tm.widthIncludingTrailingWhitespace, 1.0f);
			g.height = tm.height;
			out.push_back(std::move(g));
		}
		return S_OK;
	}

	// Replaces the sideways runs of every group by one run holding the group set horizontally,
	// centred in the group's cell and squeezed to one em wide when it is wider.
	HRESULT PlaceTcy(const TextObject& obj, const std::vector<TcyGroup>& groups, std::vector<Run>& runs)
	{
		for (const TcyGroup& g : groups) {
			D2D1_RECT_F cell = D2D1::RectF(1e9f, 1e9f, -1e9f, -1e9f);
			int span = -1;
			bool found = false;
			std::vector<Run> kept;
			for (Run& r : runs) {
				if (r.ch < g.start || r.ch >= g.start + g.length) { kept.push_back(std::move(r)); continue; }
				if (!found) span = r.span;
				found = true;
				cell = D2D1::RectF(std::min(cell.left, r.cell.left), std::min(cell.top, r.cell.top),
					std::max(cell.right, r.cell.right), std::max(cell.bottom, r.cell.bottom));
			}
			runs = std::move(kept);
			if (!found) continue;
			cell.bottom = cell.top + g.em; // a squeezed group's last advance is negative

			TextObject plain; // the group's own layout carries no span tags
			OutlineCollector collector(factory_.Get(), nullptr, plain);
			HRESULT hr = g.layout->Draw(nullptr, &collector, 0.0f, 0.0f);
			if (FAILED(hr)) return hr;
			std::vector<ID2D1Geometry*> parts;
			for (auto& r : collector.runs) parts.push_back(r.geometry.Get());
			if (parts.empty()) continue;
			ComPtr<ID2D1GeometryGroup> group;
			hr = factory_->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, parts.data(), (UINT32)parts.size(), &group);
			if (FAILED(hr)) return hr;

			// Baseline shift is "up" in the run's frame, which is to the right in vertical text.
			const float shift = span >= 0 && span < (int)obj.spans.size() ? obj.spans[span].baselineShift : 0.0f;
			const float squeeze = std::min(1.0f, g.em / g.width);
			const float cx = (cell.left + cell.right) * 0.5f + shift, cy = (cell.top + cell.bottom) * 0.5f;
			ComPtr<ID2D1TransformedGeometry> placed;
			hr = factory_->CreateTransformedGeometry(group.Get(), D2D1::Matrix3x2F::Translation(-g.width * 0.5f, -g.height * 0.5f)
				* D2D1::Matrix3x2F::Scale(squeeze, 1.0f) * D2D1::Matrix3x2F::Translation(cx, cy), &placed);
			if (FAILED(hr)) return hr;

			Run r;
			r.geometry = placed;
			r.placed = placed;
			r.span = span;
			r.ch = g.start;
			r.cell = cell;
			runs.push_back(std::move(r));
		}
		return S_OK;
	}

	// ------------------------------------------------------------ ruby
	// Adds one run per ruby group: the ruby text (vertical in vertical text) centred over the
	// group's characters, above them (horizontal) or to their right (vertical). The run animates
	// with the group's first character.
	HRESULT PlaceRuby(const TextObject& obj, std::vector<Run>& runs)
	{
		const int n = (int)obj.text.size();
		std::vector<int> spanOf(n, -1); // last span with ruby over each character
		for (size_t i = 0; i < obj.spans.size(); ++i) {
			const Span& s = obj.spans[i];
			if (s.ruby.empty()) continue;
			for (int c = std::max(s.start, 0); c < s.start + s.length && c < n; ++c) spanOf[c] = (int)i;
		}
		auto rubyAt = [&](int c) -> const std::wstring* {
			return spanOf[c] >= 0 && obj.text[c] != L'\n' && obj.text[c] != L'\r' ? &obj.spans[spanOf[c]].ruby : nullptr;
		};
		// Outer strokes of ruby and base would touch: keep them one stroke width further apart.
		float stroke = 0;
		for (const Layer& l : obj.stack)
			if (l.enabled && l.type == LayerType::Stroke && l.position != StrokePosition::Inner)
				stroke = std::max(stroke, l.position == StrokePosition::Outer ? l.width : l.width * 0.5f);
		std::vector<Run> added;
		for (int a = 0; a < n;) {
			const std::wstring* ruby = rubyAt(a);
			if (!ruby) { ++a; continue; }
			int b = a + 1;
			while (b < n && rubyAt(b) && *rubyAt(b) == *ruby) ++b;

			// Base: the cells of the group's characters.
			D2D1_RECT_F base = D2D1::RectF(1e9f, 1e9f, -1e9f, -1e9f), firstCell{};
			int span = -1, firstCh = INT_MAX;
			for (const Run& r : runs) {
				if (r.ch < a || r.ch >= b) continue;
				if (r.ch < firstCh) { firstCh = r.ch; firstCell = r.cell; span = r.span; }
				base = D2D1::RectF(std::min(base.left, r.cell.left), std::min(base.top, r.cell.top),
					std::max(base.right, r.cell.right), std::max(base.bottom, r.cell.bottom));
			}
			const int start = a;
			a = b;
			if (base.left > base.right) continue; // only blanks under the ruby

			ComPtr<IDWriteTextFormat> format;
			float em = 0;
			HRESULT hr = FormatAt(obj, start, obj.rubyScale / 100.0f, format, em);
			if (FAILED(hr)) return hr;
			format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
			if (obj.vertical) SetVertical(format.Get());
			ComPtr<IDWriteTextLayout> layout;
			hr = dwrite_->CreateTextLayout(ruby->c_str(), (UINT32)ruby->size(), format.Get(), 100000.0f, 100000.0f, &layout);
			if (FAILED(hr)) return hr;
			DWRITE_TEXT_METRICS tm{};
			layout->GetMetrics(&tm);
			TextObject plain; // the ruby layout carries no span tags
			OutlineCollector collector(factory_.Get(), analyzer2_.Get(), plain);
			hr = layout->Draw(nullptr, &collector, 0.0f, 0.0f);
			if (FAILED(hr)) return hr;
			std::vector<ID2D1Geometry*> parts;
			for (auto& r : collector.runs) parts.push_back(r.geometry.Get());
			if (parts.empty()) continue;
			ComPtr<ID2D1GeometryGroup> group;
			hr = factory_->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, parts.data(), (UINT32)parts.size(), &group);
			if (FAILED(hr)) return hr;

			// Centre of the ruby layout box goes one half base em + one half ruby em away from the
			// centre of the base characters.
			const float rubyEm = em * obj.rubyScale / 100.0f;
			const float away = (em + rubyEm) * 0.5f + stroke + obj.rubyGap;
			const float bx = (base.left + base.right) * 0.5f, by = (base.top + base.bottom) * 0.5f;
			const float tx = obj.vertical ? bx + away : bx, ty = obj.vertical ? by : by - away;
			const float lx = tm.left + tm.width * 0.5f, ly = tm.top + tm.height * 0.5f;
			ComPtr<ID2D1TransformedGeometry> placed;
			hr = factory_->CreateTransformedGeometry(group.Get(), D2D1::Matrix3x2F::Translation(tx - lx, ty - ly), &placed);
			if (FAILED(hr)) return hr;

			Run r;
			r.geometry = placed;
			r.placed = placed;
			r.span = span;
			r.ch = firstCh;
			r.ruby = true;
			r.cell = firstCell; // animates around the first character's centre, like the character itself
			added.push_back(std::move(r));
		}
		for (auto& r : added) runs.push_back(std::move(r));
		return S_OK;
	}

	// ------------------------------------------------------------ optical kerning
	// Ink extent of one glyph along the reading direction, sampled in bands across it.
	static constexpr int kRows = 24;
	struct Profile {
		bool ink = false;
		float lo[kRows], hi[kRows]; // extent per band (u = along the line); lo > hi = no ink in the band
		float bboxLo = 1e30f, bboxHi = -1e30f;
	};

	static HRESULT MakeProfile(ID2D1Geometry* g, const D2D1_RECT_F& cell, bool vertical, Profile& p)
	{
		PolySink polys;
		HRESULT hr = g->Simplify(D2D1_GEOMETRY_SIMPLIFICATION_OPTION_LINES, nullptr, 0.1f, &polys);
		if (FAILED(hr)) return hr;
		const float v0 = vertical ? cell.left : cell.top, v1 = vertical ? cell.right : cell.bottom;
		const float step = (v1 - v0) / kRows;
		for (int k = 0; k < kRows; ++k) { p.lo[k] = 1e30f; p.hi[k] = -1e30f; }
		for (const Poly& f : polys.figures) {
			const size_t n = f.size();
			for (size_t i = 0; i < n; ++i) {
				const D2D1_POINT_2F a = f[i], b = f[(i + 1) % n];
				const float au = vertical ? a.y : a.x, av = vertical ? a.x : a.y;
				const float bu = vertical ? b.y : b.x, bv = vertical ? b.x : b.y;
				p.bboxLo = std::min(p.bboxLo, au);
				p.bboxHi = std::max(p.bboxHi, au);
				if (av == bv || step <= 0) continue;
				for (int k = 0; k < kRows; ++k) {
					const float vk = v0 + (k + 0.5f) * step;
					if ((vk - av) * (vk - bv) > 0) continue;
					const float u = au + (vk - av) / (bv - av) * (bu - au);
					p.lo[k] = std::min(p.lo[k], u);
					p.hi[k] = std::max(p.hi[k], u);
				}
			}
		}
		p.ink = p.bboxLo < p.bboxHi;
		return S_OK;
	}

	// Extra spacing after each character (UTF-16 index) that evens out the visible gaps between
	// neighbouring glyphs: the gap of a pair is the average clearance between the two outlines over
	// the height of the line; pairs wider than the text's average close up, tighter ones open.
	HRESULT OpticalOffsets(const TextObject& obj, std::vector<float>& out)
	{
		const int len = (int)obj.text.size();
		out.assign(len, 0.0f);

		std::wstring key = obj.text + L"|" + obj.family + L"|" + obj.tcyChars;
		wchar_t buf[160];
		swprintf_s(buf, L"|%g|%d|%d|%g|%g|%d|%d|%g|%d", obj.size, obj.weight, obj.italic ? 1 : 0, obj.tracking, obj.lineHeight,
			obj.vertical ? 1 : 0, (int)obj.kernMode, obj.opticalKern, obj.tcyDigits);
		key += buf;
		for (const Span& s : obj.spans) {
			swprintf_s(buf, L"|s%d,%d,%g,%d,%g,%d,", s.start, s.length, s.size, s.weight, s.tracking, s.tcy ? 1 : 0);
			key += buf + s.family;
		}
		for (const auto& k : obj.kerning) {
			swprintf_s(buf, L"|k%d,%g", k.first, k.second);
			key += buf;
		}
		for (const auto& c : opticalCache_)
			if (c.first == key) { out = c.second; return S_OK; }

		TextObject plain = obj;
		plain.opticalKern = 0;
		TextShape sh;
		HRESULT hr = BuildShape(plain, 1.0f, sh);
		if (FAILED(hr)) return hr;

		struct Item { int ch; Profile p; };
		std::vector<Item> items;
		for (const Run& r : sh.runs) {
			if (r.ruby || !r.geometry) continue;
			Item it;
			it.ch = r.ch;
			hr = MakeProfile(r.geometry.Get(), r.cell, obj.vertical, it.p);
			if (FAILED(hr)) return hr;
			items.push_back(std::move(it));
		}
		std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.ch < b.ch; });
		auto emAt = [&](int ch) {
			float s = obj.size;
			for (const Span& sp : obj.spans)
				if (ch >= sp.start && ch < sp.start + sp.length && sp.size > 0) s = sp.size;
			return std::max(s, 1.0f);
		};

		// Pairs are only compared with pairs of the same kinds of characters (CJK next to CJK, Latin next
		// to Latin, ...): the usual gap of kana would otherwise open up every tight Latin pair.
		auto kind = [&](int ch) { return ch >= 0 && ch < len && (unsigned)obj.text[ch] >= 0x2E80 ? 1 : 0; };
		struct Pair { int index; float gap, em; int group; };
		std::vector<Pair> pairs;
		for (size_t i = 0; i + 1 < items.size(); ++i) {
			const Item& a = items[i], & b = items[i + 1];
			if (!a.p.ink || !b.p.ink || b.ch <= a.ch) continue;
			bool lineBreak = false;
			for (int c = a.ch; c < b.ch && c < len; ++c)
				if (obj.text[c] == L'\n' || obj.text[c] == L'\r') lineBreak = true;
			if (lineBreak) continue;
			const float em = emAt(a.ch);
			float sum = 0;
			int rows = 0;
			for (int k = 0; k < kRows; ++k) {
				const bool ha = a.p.lo[k] <= a.p.hi[k], hb = b.p.lo[k] <= b.p.hi[k];
				if (!ha && !hb) continue;
				float c;
				if (ha && hb) c = b.p.lo[k] - a.p.hi[k];
				else if (ha) c = b.p.bboxLo - a.p.hi[k];
				else c = b.p.lo[k] - a.p.bboxHi;
				sum += std::clamp(c, -0.3f * em, 0.5f * em);
				++rows;
			}
			if (rows == 0) continue;
			pairs.push_back({ b.ch - 1, sum / rows, em, kind(a.ch) * 2 + kind(b.ch) });
		}
		const float strength = obj.opticalKern / 100.0f;
		for (int group = 0; group < 4; ++group) {
			float mean = 0;
			int count = 0;
			for (const Pair& p : pairs)
				if (p.group == group) { mean += p.gap; ++count; }
			if (count < 2) continue;
			mean /= count;
			for (const Pair& p : pairs)
				if (p.group == group && p.index >= 0 && p.index < len)
					out[p.index] += std::clamp((mean - p.gap) * strength, -0.25f * p.em, 0.25f * p.em);
		}
		if (opticalCache_.size() >= 8) opticalCache_.erase(opticalCache_.begin());
		opticalCache_.push_back({ key, out });
		return S_OK;
	}

	HRESULT MakeUnion(TextShape& shape)
	{
		std::vector<ID2D1Geometry*> raw;
		for (auto& r : shape.runs) raw.push_back(r.placed.Get());
		shape.all.Reset();
		if (raw.empty()) return S_OK;
		ComPtr<ID2D1GeometryGroup> group;
		HRESULT hr = factory_->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, raw.data(), (UINT32)raw.size(), &group);
		if (FAILED(hr)) return hr;
		shape.all = group;
		return S_OK;
	}

	static bool IsBlank(wchar_t c)
	{
		return c == L' ' || c == L'　' || c == L'\t' || c == L'\r' || c == L'\n';
	}

	// Line (column in vertical text) of every character, plus one past the end.
	static std::vector<int> LineOfChars(IDWriteTextLayout* layout, size_t length, int& count)
	{
		std::vector<int> lineOf(length + 1, 0);
		UINT32 lineCount = 0;
		layout->GetLineMetrics(nullptr, 0, &lineCount);
		std::vector<DWRITE_LINE_METRICS> lines(lineCount);
		if (lineCount && SUCCEEDED(layout->GetLineMetrics(lines.data(), lineCount, &lineCount))) {
			size_t pos = 0;
			for (UINT32 l = 0; l < lineCount; ++l)
				for (UINT32 c = 0; c < lines[l].length && pos < lineOf.size(); ++c) lineOf[pos++] = (int)l;
		}
		count = std::max<int>(lineCount, 1);
		return lineOf;
	}

	// Paint area of every run: the ink of its line (ruby left out), or the whole object.
	void SetPaintBoxes(const TextObject& obj, IDWriteTextLayout* layout, TextShape& shape)
	{
		if (!obj.paintPerLine) {
			for (Run& r : shape.runs) r.paintBox = shape.ink;
			return;
		}
		int count = 0;
		const std::vector<int> lineOf = LineOfChars(layout, obj.text.size(), count);
		auto lineAt = [&](const Run& r) { return r.ch >= 0 && r.ch < (int)lineOf.size() ? std::clamp(lineOf[r.ch], 0, count - 1) : 0; };
		std::vector<D2D1_RECT_F> box(count, D2D1::RectF(1e9f, 1e9f, -1e9f, -1e9f));
		for (const Run& r : shape.runs) {
			if (r.ruby) continue;
			D2D1_RECT_F b{};
			r.geometry->GetBounds(nullptr, &b);
			if (b.left > b.right) continue; // blank
			D2D1_RECT_F& l = box[lineAt(r)];
			l = D2D1::RectF(std::min(l.left, b.left), std::min(l.top, b.top), std::max(l.right, b.right), std::max(l.bottom, b.bottom));
		}
		for (Run& r : shape.runs) {
			const D2D1_RECT_F& l = box[lineAt(r)];
			r.paintBox = l.left <= l.right ? l : shape.ink;
		}
	}

	// Gives every run its animated transform / opacity for this frame.
	HRESULT Animate(const TextObject& obj, IDWriteTextLayout* layout, int frame, int durationFrames, TextShape& shape)
	{
		// Line of every character.
		int lineCount = 0;
		const std::vector<int> lineOf = LineOfChars(layout, obj.text.size(), lineCount);
		// Reading order of the visible characters.
		std::vector<int> chars;
		for (const Run& r : shape.runs)
			if (r.ch >= 0 && r.ch < (int)obj.text.size() && !IsBlank(obj.text[r.ch])) chars.push_back(r.ch);
		std::sort(chars.begin(), chars.end());
		chars.erase(std::unique(chars.begin(), chars.end()), chars.end());

		struct Units {
			std::vector<int> unitOfRun;
			std::vector<D2D1_RECT_F> bounds;
			int count = 0;
		};
		auto build = [&](const AnimSpec& a) {
			Units u;
			u.unitOfRun.assign(shape.runs.size(), -1);
			for (size_t i = 0; i < shape.runs.size(); ++i) {
				const int ch = shape.runs[i].ch;
				if (ch < 0 || ch >= (int)obj.text.size() || IsBlank(obj.text[ch])) continue;
				int unit = 0;
				if (a.unit == AnimUnit::Char) unit = (int)(std::lower_bound(chars.begin(), chars.end(), ch) - chars.begin());
				else if (a.unit == AnimUnit::Line) unit = lineOf[ch];
				u.unitOfRun[i] = unit;
				u.count = std::max(u.count, unit + 1);
			}
			u.bounds.assign(u.count, D2D1::RectF(1e9f, 1e9f, -1e9f, -1e9f));
			for (size_t i = 0; i < shape.runs.size(); ++i) {
				const int unit = u.unitOfRun[i];
				if (unit < 0) continue;
				const D2D1_RECT_F& c = shape.runs[i].cell;
				D2D1_RECT_F& b = u.bounds[unit];
				b = D2D1::RectF(std::min(b.left, c.left), std::min(b.top, c.top), std::max(b.right, c.right), std::max(b.bottom, c.bottom));
			}
			return u;
		};
		const Units ui = build(obj.animIn), uo = build(obj.animOut);

		// With a 3D layer the 3D turns are drawn for real by that layer; without one they squash the unit.
		const bool real3D = Extrude3DIndex(obj) >= 0;
		auto squash = [](float deg) {
			const float c = std::cos(deg * kPi / 180.0f);
			return std::fabs(c) < 0.001f ? (c < 0 ? -0.001f : 0.001f) : c;
		};
		auto motion = [&](const UnitState& s, const D2D1_RECT_F& b) {
			// Pivot: the unit's centre, or one of its edges (hinge).
			const float cx = (b.left + b.right) * 0.5f + s.px * (b.right - b.left), cy = (b.top + b.bottom) * 0.5f + s.py * (b.bottom - b.top);
			float k = std::max(s.scale, 0.001f); // keep the matrix invertible
			if (!real3D && s.dz != 0) { // without a 3D layer, depth only shows as the perspective size
				const float D = docW_ * 1.39f;
				k *= D / std::max(D + s.dz, D * 0.05f);
			}
			const float kx = real3D ? k : k * squash(s.rotY), ky = real3D ? k : k * squash(s.rotX);
			return D2D1::Matrix3x2F::Translation(-cx, -cy) * D2D1::Matrix3x2F::Scale(kx, ky)
				* D2D1::Matrix3x2F::Rotation(s.rotation) * D2D1::Matrix3x2F::Translation(cx + s.dx, cy + s.dy);
		};
		// The background box belongs to the whole object: it only follows "all" animations.
		D2D1::Matrix3x2F bg = D2D1::Matrix3x2F::Identity();
		if (obj.animIn.unit == AnimUnit::All && ui.count > 0)
			bg = bg * motion(EvalAnim(obj.animIn, false, 0, 1, frame, durationFrames), ui.bounds[0]);
		if (obj.animOut.unit == AnimUnit::All && uo.count > 0)
			bg = bg * motion(EvalAnim(obj.animOut, true, 0, 1, frame, durationFrames), uo.bounds[0]);
		shape.bgM = bg;

		for (size_t i = 0; i < shape.runs.size(); ++i) {
			Run& r = shape.runs[i];
			if (ui.unitOfRun[i] < 0) continue; // blank: nothing to draw
			const UnitState si = EvalAnim(obj.animIn, false, ui.unitOfRun[i], ui.count, frame, durationFrames);
			const UnitState so = EvalAnim(obj.animOut, true, uo.unitOfRun[i], uo.count, frame, durationFrames);
			const D2D1::Matrix3x2F m = motion(si, ui.bounds[ui.unitOfRun[i]]) * motion(so, uo.bounds[uo.unitOfRun[i]]);
			r.m = m;
			r.opacity = std::clamp(si.opacity * so.opacity, 0.0f, 1.0f);
			r.rotX = si.rotX + so.rotX;
			r.rotY = si.rotY + so.rotY;
			r.dz = si.dz + so.dz;
			r.depth = si.depth * so.depth;
			// The 3D turn pivots around the centre (or hinge edge) of the unit of the animation that turns.
			const bool inTurns = obj.animIn.type == AnimType::Flip3D || obj.animIn.type == AnimType::Hinge || obj.animIn.type == AnimType::Tumble;
			const D2D1_RECT_F& pb = inTurns ? ui.bounds[ui.unitOfRun[i]] : uo.bounds[uo.unitOfRun[i]];
			const UnitState& ps = inTurns ? si : so;
			r.pivot = m.TransformPoint(D2D1::Point2F((pb.left + pb.right) * 0.5f + ps.px * (pb.right - pb.left),
				(pb.top + pb.bottom) * 0.5f + ps.py * (pb.bottom - pb.top)));
			if (!m.IsIdentity()) {
				ComPtr<ID2D1TransformedGeometry> t;
				HRESULT hr = factory_->CreateTransformedGeometry(r.geometry.Get(), m, &t);
				if (FAILED(hr)) return hr;
				r.placed = t;
			}
		}
		return MakeUnion(shape);
	}

	// ------------------------------------------------------------ paints
	HRESULT MakeTile(int w, int h, const Paint& p, float deviceScale, ComPtr<ID2D1Bitmap>& out)
	{
		ComPtr<ID2D1BitmapRenderTarget> rt;
		HRESULT hr = dc_->CreateCompatibleRenderTarget(D2D1::SizeF((float)w, (float)h), D2D1::SizeU(w, h),
			D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &rt);
		if (FAILED(hr)) return hr;
		ComPtr<ID2D1SolidColorBrush> ink;
		rt->CreateSolidColorBrush(ToColor(p.color), &ink);
		rt->BeginDraw();
		rt->Clear(ToColor(p.color2));
		if (p.type == PaintType::Stripe) {
			const float sw = std::max(p.size * deviceScale, 0.5f);
			rt->FillRectangle(D2D1::RectF(0, 0, sw, (float)h), ink.Get());
		} else {
			const float r = std::max(p.size * deviceScale * 0.5f, 0.5f);
			rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(w * 0.5f, h * 0.5f), r, r), ink.Get());
		}
		hr = rt->EndDraw();
		if (FAILED(hr)) return hr;
		return rt->GetBitmap(&out);
	}

	// Brush in layout space; bounds = area the gradient / pattern is fitted to.
	HRESULT MakeBrush(const Paint& p, const D2D1_RECT_F& bounds, float deviceScale, ComPtr<ID2D1Brush>& out)
	{
		HRESULT hr = S_OK;
		switch (p.type) {
		case PaintType::Solid: {
			ComPtr<ID2D1SolidColorBrush> b;
			hr = dc_->CreateSolidColorBrush(ToColor(p.color), &b);
			out = b;
			return hr;
		}
		case PaintType::Texture: {
			// The texture itself is laid over the layer afterwards (LayTexture); this is what is underneath.
			if (p.base) return MakeBrush(*p.base, bounds, deviceScale, out);
			ComPtr<ID2D1SolidColorBrush> b;
			hr = dc_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), &b);
			out = b;
			return hr;
		}
		case PaintType::Linear:
		case PaintType::Radial: {
			std::vector<D2D1_GRADIENT_STOP> stops;
			for (const auto& s : p.stops) stops.push_back({ std::clamp(s.pos, 0.0f, 1.0f), ToColor(s.color) });
			std::sort(stops.begin(), stops.end(), [](const auto& a, const auto& b) { return a.position < b.position; });
			if (stops.empty()) stops = { { 0, D2D1::ColorF(1, 1, 1, 1) }, { 1, D2D1::ColorF(0, 0, 0, 1) } };
			ComPtr<ID2D1GradientStopCollection> coll;
			hr = dc_->CreateGradientStopCollection(stops.data(), (UINT32)stops.size(), D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &coll);
			if (FAILED(hr)) return hr;
			const float cx = (bounds.left + bounds.right) * 0.5f, cy = (bounds.top + bounds.bottom) * 0.5f;
			const float hw = (bounds.right - bounds.left) * 0.5f, hh = (bounds.bottom - bounds.top) * 0.5f;
			if (p.type == PaintType::Linear) {
				const float a = p.angle * kPi / 180.0f, dx = std::cos(a), dy = std::sin(a);
				const float ext = std::fabs(hw * dx) + std::fabs(hh * dy);
				ComPtr<ID2D1LinearGradientBrush> b;
				hr = dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
					D2D1::Point2F(cx - dx * ext, cy - dy * ext), D2D1::Point2F(cx + dx * ext, cy + dy * ext)), coll.Get(), &b);
				out = b;
			} else {
				ComPtr<ID2D1RadialGradientBrush> b;
				hr = dc_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(
					D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), std::max(hw, 1.0f), std::max(hh, 1.0f)), coll.Get(), &b);
				out = b;
			}
			return hr;
		}
		case PaintType::Stripe:
		case PaintType::Dots: {
			// The tile is rendered at device resolution; the brush transform maps it back to layout units.
			const float period = std::max(p.type == PaintType::Stripe ? p.size + p.gap : p.gap, 1.0f);
			const int tilePx = std::clamp((int)std::lround(period * deviceScale), 2, 2048);
			ComPtr<ID2D1Bitmap> tile;
			hr = MakeTile(tilePx, tilePx, p, deviceScale, tile);
			if (FAILED(hr)) return hr;
			ComPtr<ID2D1BitmapBrush> b;
			hr = dc_->CreateBitmapBrush(tile.Get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
				D2D1_BITMAP_INTERPOLATION_MODE_LINEAR), &b);
			if (FAILED(hr)) return hr;
			const float k = period / tilePx;
			b->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Rotation(p.angle)
				* D2D1::Matrix3x2F::Translation(bounds.left, bounds.top));
			out = b;
			return S_OK;
		}
		}
		return E_INVALIDARG;
	}

	const Paint& PaintFor(const TextObject& obj, const Layer& layer, int span)
	{
		if (span >= 0 && span < (int)obj.spans.size())
			for (const auto& o : obj.spans[span].paints)
				if (o.layerId == layer.id) return o.paint;
		return layer.paint;
	}

	// ------------------------------------------------------------ layers
	ComPtr<ID2D1StrokeStyle> StrokeStyle(const Layer& l)
	{
		const D2D1_LINE_JOIN join = l.join == LineJoin::Miter ? D2D1_LINE_JOIN_MITER_OR_BEVEL
			: l.join == LineJoin::Bevel ? D2D1_LINE_JOIN_BEVEL : D2D1_LINE_JOIN_ROUND;
		ComPtr<ID2D1StrokeStyle> style;
		factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
			join, std::max(l.miterLimit, 1.0f)), nullptr, 0, &style);
		return style;
	}

	// Draws a vector layer (fill / stroke / background) into the current target with the layout transform set.
	// forced: draw everything with this brush instead of the layer's paints (the layer's coverage as a mask).
	HRESULT DrawVectorLayer(const TextObject& obj, const Layer& l, const TextShape& shape, ID2D1Brush* forced = nullptr)
	{
		HRESULT hr = S_OK;
		const D2D1::Matrix3x2F layout = *D2D1::Matrix3x2F::ReinterpretBaseType(&shape.toOutput);
		// Glyphs are drawn at rest with their animation transform, so paints and stroke widths move with them.
		auto place = [&](const Run& run) { dc_->SetTransform(*D2D1::Matrix3x2F::ReinterpretBaseType(&run.m) * layout); };
		if (l.type == LayerType::Background) {
			if (!shape.background) return S_OK;
			const D2D1_RECT_F r = Inflate(shape.ink, l.padX, l.padY);
			ComPtr<ID2D1Brush> brush;
			hr = MakeBrush(l.paint, r, shape.deviceScale, brush);
			if (FAILED(hr)) return hr;
			if (forced) brush = forced;
			dc_->SetTransform(*D2D1::Matrix3x2F::ReinterpretBaseType(&shape.bgM) * layout);
			dc_->FillRoundedRectangle(D2D1::RoundedRect(r, l.radius, l.radius), brush.Get());
			dc_->SetTransform(layout);
			return S_OK;
		}
		if (!shape.all) return S_OK;

		if (l.type == LayerType::Fill) {
			for (const Run& run : shape.runs) {
				ComPtr<ID2D1Brush> brush;
				hr = MakeBrush(PaintFor(obj, l, run.span), run.paintBox, shape.deviceScale, brush);
				if (FAILED(hr)) break;
				if (forced) brush = forced;
				place(run);
				dc_->FillGeometry(run.geometry.Get(), brush.Get());
			}
			dc_->SetTransform(layout);
			return hr;
		}

		// Stroke
		if (l.width <= 0) return S_OK;
		ComPtr<ID2D1StrokeStyle> style = StrokeStyle(l);
		const float grow = l.position == StrokePosition::Outer ? l.width : l.position == StrokePosition::Center ? l.width * 0.5f : 0;
		const bool inner = l.position == StrokePosition::Inner;
		if (inner)
			dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), shape.all.Get()), nullptr);
		for (const Run& run : shape.runs) {
			ComPtr<ID2D1Brush> brush;
			hr = MakeBrush(PaintFor(obj, l, run.span), Inflate(run.paintBox, grow, grow), shape.deviceScale, brush);
			if (FAILED(hr)) break;
			if (forced) brush = forced;
			const float strokeWidth = l.position == StrokePosition::Center ? l.width : l.width * 2.0f;
			place(run);
			dc_->DrawGeometry(run.geometry.Get(), brush.Get(), strokeWidth, style.Get());
			// Outer stroke = the glyph grown by width: include the inside too, so its order in the stack is free.
			if (l.position == StrokePosition::Outer) dc_->FillGeometry(run.geometry.Get(), brush.Get());
		}
		dc_->SetTransform(layout);
		if (inner) dc_->PopLayer();
		return hr;
	}

	ComPtr<ID2D1Image> ShadowOf(ID2D1Image* input, uint32_t color, float blurPx, float spreadPx, bool erode)
	{
		ComPtr<ID2D1Image> src = input;
		if (spreadPx >= 0.5f) {
			ComPtr<ID2D1Effect> morph;
			if (SUCCEEDED(dc_->CreateEffect(CLSID_D2D1Morphology, &morph))) {
				const UINT k = (UINT)std::clamp((int)std::lround(spreadPx * 2 + 1), 1, 100);
				morph->SetInput(0, src.Get());
				morph->SetValue(D2D1_MORPHOLOGY_PROP_MODE, erode ? D2D1_MORPHOLOGY_MODE_ERODE : D2D1_MORPHOLOGY_MODE_DILATE);
				morph->SetValue(D2D1_MORPHOLOGY_PROP_WIDTH, k);
				morph->SetValue(D2D1_MORPHOLOGY_PROP_HEIGHT, k);
				morph->GetOutput(&src);
			}
		}
		ComPtr<ID2D1Effect> shadow;
		if (FAILED(dc_->CreateEffect(CLSID_D2D1Shadow, &shadow))) return nullptr;
		shadow->SetInput(0, src.Get());
		shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, std::max(blurPx * 0.5f, 0.0f));
		shadow->SetValue(D2D1_SHADOW_PROP_COLOR, ToVector(color));
		shadow->SetValue(D2D1_SHADOW_PROP_OPTIMIZATION, D2D1_SHADOW_OPTIMIZATION_QUALITY);
		ComPtr<ID2D1Image> out;
		shadow->GetOutput(&out);
		return out;
	}

	ID2D1Bitmap1* Slot(int i) { return slots_[i].Get(); }

	void Target(int slot, bool clear)
	{
		dc_->SetTarget(Slot(slot));
		if (clear) dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
	}

	// Renders one layer into kTmp (cleared). kAccum holds every layer in front of it.
	HRESULT RenderLayer(const TextObject& obj, const Layer& l, const TextShape& shape)
	{
		const float s = shape.deviceScale;
		const float a = l.angle * kPi / 180.0f;
		const D2D1_POINT_2F dir = D2D1::Point2F(std::cos(a), std::sin(a));
		const D2D1::Matrix3x2F layout = *D2D1::Matrix3x2F::ReinterpretBaseType(&shape.toOutput);
		const auto layerParams = D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
			D2D1::IdentityMatrix(), std::clamp(l.opacity / 100.0f, 0.0f, 1.0f));

		switch (l.type) {
		case LayerType::Fill:
		case LayerType::Stroke:
		case LayerType::Background: {
			Target(kTmp, true);
			dc_->SetTransform(layout);
			dc_->PushLayer(layerParams, nullptr);
			HRESULT hr = DrawVectorLayer(obj, l, shape);
			dc_->PopLayer();
			dc_->SetTransform(D2D1::IdentityMatrix());
			if (SUCCEEDED(hr) && l.paint.type == PaintType::Texture && !l.paint.tex.path.empty()) {
				// The texture goes over what the base paint drew, inside the layer's own shape.
				ComPtr<ID2D1SolidColorBrush> white;
				hr = dc_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
				if (SUCCEEDED(hr)) {
					Target(kA, true);
					dc_->SetTransform(layout);
					hr = DrawVectorLayer(obj, l, shape, white.Get());
					dc_->SetTransform(D2D1::IdentityMatrix());
				}
				if (SUCCEEDED(hr))
					hr = LayTexture(l.paint.tex, shape, kTmp, kA, std::clamp(l.opacity / 100.0f, 0.0f, 1.0f) * l.paint.tex.opacity / 100.0f);
			}
			return hr;
		}
		case LayerType::Shadow: {
			Target(kTmp, true);
			ComPtr<ID2D1Image> sh = ShadowOf(Slot(kAccum), l.color, l.blur * s, l.spread * s, false);
			if (!sh) return E_FAIL;
			dc_->PushLayer(layerParams, nullptr);
			dc_->DrawImage(sh.Get(), D2D1::Point2F(dir.x * l.distance * s, dir.y * l.distance * s));
			dc_->PopLayer();
			return S_OK;
		}
		case LayerType::InnerShadow: {
			if (!shape.all) { Target(kTmp, true); return S_OK; }
			// A = glyph silhouette, B = everything outside it.
			Target(kA, true);
			ComPtr<ID2D1SolidColorBrush> white;
			dc_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
			dc_->SetTransform(layout);
			dc_->FillGeometry(shape.all.Get(), white.Get());
			dc_->SetTransform(D2D1::IdentityMatrix());
			Target(kB, false);
			dc_->Clear(D2D1::ColorF(0, 0, 0, 1));
			dc_->DrawImage(Slot(kA), Origin(), D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_DESTINATION_OUT);

			Target(kTmp, true);
			ComPtr<ID2D1Image> sh = ShadowOf(Slot(kB), l.color, l.blur * s, l.spread * s, false);
			if (!sh) return E_FAIL;
			dc_->PushLayer(layerParams, nullptr);
			dc_->DrawImage(sh.Get(), D2D1::Point2F(dir.x * l.distance * s, dir.y * l.distance * s));
			dc_->DrawImage(Slot(kA), Origin(), D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_DESTINATION_IN);
			dc_->PopLayer();
			return S_OK;
		}
		case LayerType::LongShadow: {
			// Union of the front layers' silhouette shifted 1..n px along the angle, built by doubling.
			int n = (int)std::lround(l.length * s);
			if (n <= 0) { Target(kTmp, true); return S_OK; }
			ComPtr<ID2D1Image> tinted = ShadowOf(Slot(kAccum), l.paint.color, 0, 0, false);
			if (!tinted) return E_FAIL;
			Target(kA, true);
			dc_->DrawImage(tinted.Get());
			int cur = kA, spare = kC;
			Target(kB, true); // result
			float offset = 1.0f, len = 1.0f;
			while (n > 0) {
				if (n & 1) {
					Target(kB, false);
					dc_->DrawImage(Slot(cur), D2D1::Point2F(dir.x * offset, dir.y * offset));
					offset += len;
				}
				n >>= 1;
				if (!n) break;
				Target(spare, true);
				dc_->DrawImage(Slot(cur));
				dc_->DrawImage(Slot(cur), D2D1::Point2F(dir.x * len, dir.y * len));
				std::swap(cur, spare);
				len *= 2;
			}
			Target(kTmp, true);
			dc_->PushLayer(layerParams, nullptr);
			dc_->DrawImage(Slot(kB));
			dc_->PopLayer();
			return S_OK;
		}
		}
		return S_OK;
	}

	static D2D1_POINT_2F Origin() { return D2D1::Point2F(0, 0); }

	// ------------------------------------------------------------ textures
	struct TexSlot {
		std::wstring key;
		ComPtr<ID2D1Bitmap> bitmap;
		int w = 0, h = 0;
	};

	// Bitmap of the texture's frame at the current frame; false (nothing to draw) if it is not ready.
	bool TextureBitmap(const Texture& t, ComPtr<ID2D1Bitmap>& bitmap, int& w, int& h)
	{
		if (t.path.empty()) return false;
		const media::Info info = media::Lookup(t.path);
		if (!info.ok) {
			if (info.video && info.error.empty() && !info.converting) media::Request(t.path); // convert it in the background
			return false;
		}
		const int index = TextureFrameIndex(t, curFrame_, curFps_, info.frames, info.fps);
		const std::wstring key = info.id + L"#" + std::to_wstring(index);
		for (const TexSlot& s : texCache_)
			if (s.key == key) { bitmap = s.bitmap; w = s.w; h = s.h; return true; }
		const std::shared_ptr<const media::Frame> f = media::GetFrame(t.path, index);
		if (!f) return false;
		ComPtr<ID2D1Bitmap> b;
		if (FAILED(dc_->CreateBitmap(D2D1::SizeU((UINT32)f->w, (UINT32)f->h), f->bgra.data(), (UINT32)f->w * 4,
			D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &b)))
			return false;
		if (texCache_.size() >= 8) texCache_.erase(texCache_.begin());
		texCache_.push_back({ key, b, f->w, f->h });
		bitmap = b;
		w = f->w;
		h = f->h;
		return true;
	}

	// Draws the texture, fitted and placed as it asks, into the current target (identity transform).
	// S_FALSE = nothing drawn (the file is not ready or unreadable).
	HRESULT DrawTextureFill(const Texture& t, const TextShape& shape)
	{
		ComPtr<ID2D1Bitmap> bitmap;
		int tw = 0, th = 0;
		if (!TextureBitmap(t, bitmap, tw, th)) return S_FALSE;
		D2D1_RECT_F box;
		D2D1::Matrix3x2F base;
		if (t.area == TexArea::Text) {
			box = shape.ink;
			base = *D2D1::Matrix3x2F::ReinterpretBaseType(&shape.toOutput);
		} else {
			box = D2D1::RectF(0, 0, (float)docW_, (float)docH_);
			base = D2D1::Matrix3x2F::Scale(outScale_, outScale_);
		}
		const float bw = std::max(box.right - box.left, 1.0f), bh = std::max(box.bottom - box.top, 1.0f);
		float sx = 1, sy = 1;
		switch (t.fit) {
		case TexFit::Cover: sx = sy = std::max(bw / tw, bh / th); break;
		case TexFit::Contain: sx = sy = std::min(bw / tw, bh / th); break;
		case TexFit::Stretch: sx = bw / tw; sy = bh / th; break;
		case TexFit::Tile: break;
		}
		const float k = std::max(t.scale, 1.0f) / 100.0f;
		sx *= k;
		sy *= k;
		const float cx = (box.left + box.right) * 0.5f + t.offsetX, cy = (box.top + box.bottom) * 0.5f + t.offsetY;
		const D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Translation(-tw * 0.5f, -th * 0.5f) * D2D1::Matrix3x2F::Scale(sx, sy)
			* D2D1::Matrix3x2F::Rotation(t.rotation) * D2D1::Matrix3x2F::Translation(cx, cy) * base;
		if (t.fit == TexFit::Contain) {
			dc_->SetTransform(m);
			dc_->DrawBitmap(bitmap.Get(), D2D1::RectF(0, 0, (float)tw, (float)th), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
			dc_->SetTransform(D2D1::IdentityMatrix());
			return S_OK;
		}
		// Cover / stretch: mirrored beyond the edges so that turning or moving never shows a gap.
		const D2D1_EXTEND_MODE ext = t.fit == TexFit::Tile ? D2D1_EXTEND_MODE_WRAP : D2D1_EXTEND_MODE_MIRROR;
		ComPtr<ID2D1BitmapBrush> brush;
		HRESULT hr = dc_->CreateBitmapBrush(bitmap.Get(), D2D1::BitmapBrushProperties(ext, ext, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR), &brush);
		if (FAILED(hr)) return hr;
		brush->SetTransform(m);
		dc_->FillRectangle(D2D1::RectF(0, 0, (float)targetW_, (float)targetH_), brush.Get());
		return S_OK;
	}

	static D2D1_BLEND_MODE BlendMode(TexBlend b)
	{
		switch (b) {
		case TexBlend::Multiply: return D2D1_BLEND_MODE_MULTIPLY;
		case TexBlend::Screen: return D2D1_BLEND_MODE_SCREEN;
		case TexBlend::Overlay: return D2D1_BLEND_MODE_OVERLAY;
		case TexBlend::Add: return D2D1_BLEND_MODE_LINEAR_DODGE;
		case TexBlend::Lighten: return D2D1_BLEND_MODE_LIGHTEN;
		case TexBlend::Darken: return D2D1_BLEND_MODE_DARKEN;
		case TexBlend::SoftLight: return D2D1_BLEND_MODE_SOFT_LIGHT;
		default: return D2D1_BLEND_MODE_MULTIPLY;
		}
	}

	// Lays the texture over slot `dst`, blended, where slot `mask` has alpha (mask < 0: everywhere).
	// Uses kA / kB / kC as scratch, so `mask` may be `dst` or kA but not kB / kC.
	HRESULT LayTexture(const Texture& t, const TextShape& shape, int dst, int mask, float opacity)
	{
		if (opacity <= 0) return S_OK;
		Target(kB, true);
		dc_->SetTransform(D2D1::IdentityMatrix());
		HRESULT hr = DrawTextureFill(t, shape);
		if (hr != S_OK) return SUCCEEDED(hr) ? S_OK : hr; // nothing to draw yet
		if (mask >= 0) dc_->DrawImage(Slot(mask), Origin(), D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_DESTINATION_IN);

		const D2D1_LAYER_PARAMETERS1 fade = D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
			D2D1::IdentityMatrix(), std::clamp(opacity, 0.0f, 1.0f));
		if (t.blend == TexBlend::Normal) {
			Target(dst, false);
			dc_->PushLayer(fade, nullptr);
			dc_->DrawImage(Slot(kB));
			dc_->PopLayer();
			return S_OK;
		}
		int src = kB;
		if (opacity < 0.999f) {
			Target(kC, true);
			dc_->PushLayer(fade, nullptr);
			dc_->DrawImage(Slot(kB));
			dc_->PopLayer();
			src = kC;
		}
		ComPtr<ID2D1Effect> blend;
		hr = dc_->CreateEffect(CLSID_D2D1Blend, &blend);
		if (FAILED(hr)) return hr;
		blend->SetInput(0, Slot(dst));
		blend->SetInput(1, Slot(src));
		blend->SetValue(D2D1_BLEND_PROP_MODE, BlendMode(t.blend));
		Target(kA, true);
		dc_->DrawImage(blend.Get());
		Target(dst, true);
		dc_->DrawImage(Slot(kA));
		return S_OK;
	}

	// ------------------------------------------------------------ surface pass (fill / stroke bevel and material)
	struct SurfaceConstants {
		float view[4];   // output width, height, camera distance, surroundings
		float bevel[4];  // width px, height px, shape, alpha threshold
		float light[4];  // towards the light, intensity
		float camera[4]; // camera position, ambient
		float mat[4];    // metallic, roughness, brushed, lit
		float jump[4];   // jump flood step px
		float rot[3][4]; // turn of the 3D layer behind (front face normal), rows
		float bump[4];   // bump height px for white, bump map present
	};

	void ResetSeeds()
	{
		for (int i = 0; i < 2; ++i) { seedTex_[i].Reset(); seedSrv_[i].Reset(); seedRtv_[i].Reset(); }
	}

	// Two full-frame float2 targets for the jump flood (nearest outside pixel).
	HRESULT EnsureSeeds()
	{
		if (seedTex_[0]) return S_OK;
		D3D11_TEXTURE2D_DESC td{};
		td.Width = (UINT)targetW_;
		td.Height = (UINT)targetH_;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R32G32_FLOAT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		for (int i = 0; i < 2; ++i) {
			HRESULT hr = device_->CreateTexture2D(&td, nullptr, &seedTex_[i]);
			if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(seedTex_[i].Get(), nullptr, &seedSrv_[i]);
			if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(seedTex_[i].Get(), nullptr, &seedRtv_[i]);
			if (FAILED(hr)) { ResetSeeds(); return hr; }
		}
		return S_OK;
	}

	// Full-frame pass: ps reads srv0 / srv1 / srv2, writes rtv.
	void FullPass(ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv, ID3D11ShaderResourceView* srv0, ID3D11ShaderResourceView* srv1,
		ID3D11ShaderResourceView* srv2 = nullptr)
	{
		ID3D11ShaderResourceView* none[3] = { nullptr, nullptr, nullptr };
		context_->PSSetShaderResources(0, 3, none); // the target may have been bound as input before
		context_->OMSetRenderTargets(1, &rtv, nullptr);
		ID3D11ShaderResourceView* srvs[3] = { srv0, srv1, srv2 };
		context_->PSSetShaderResources(0, 3, srvs);
		context_->PSSetShader(ps, nullptr, 0);
		context_->Draw(3, 0);
	}

	// Re-draws the layer in kTmp lit: bevel shading from the distance to its edge, lit material.
	HRESULT ApplySurface(const TextObject& obj, const Layer& l, const TextShape& shape)
	{
		if (!Init3D()) return S_OK; // no shaders on this device: keep the flat layer
		HRESULT hr = EnsureSeeds();
		if (FAILED(hr)) return hr;
		const float W = (float)targetW_, H = (float)targetH_;
		const float ds = shape.deviceScale;
		const bool bevel = l.bevel != BevelShape::None && l.bevelWidth > 0 && l.bevelHeight > 0;
		const float widthPx = std::max(l.bevelWidth * ds, 0.5f);
		const float D = W * 1.39f;
		const float4v ld = LightVector(curLight_);

		// Bump: the texture drawn into kC as a height map (its brightness) in output pixels.
		bool bump = false;
		if (!l.bump.path.empty() && l.bumpDepth != 0) {
			Target(kC, true);
			dc_->SetTransform(D2D1::IdentityMatrix());
			bump = DrawTextureFill(l.bump, shape) == S_OK;
		}
		SurfaceConstants c = {
			{ W, H, D, curLight_.env == EnvStyle::Studio ? 1.0f : 0.0f },
			{ widthPx, l.bevelHeight * ds, bevel ? (float)(int)l.bevel : 0.0f, 0.5f * std::clamp(l.opacity / 100.0f, 0.02f, 1.0f) },
			{ ld.v[0], ld.v[1], ld.v[2], ld.v[3] },
			{ W * 0.5f, H * 0.5f, -D, std::max(curLight_.ambient, 0.0f) / 100.0f },
			{ std::clamp(l.metallic / 100.0f, 0.0f, 1.0f), std::clamp(l.roughness / 100.0f, 0.0f, 1.0f), l.brushed ? 1.0f : 0.0f,
				l.material == Material::Lit ? 1.0f : 0.0f },
			{ 1, 0, 0, 0 },
			{ { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } },
			{ l.bumpDepth * ds, bump ? 1.0f : 0.0f, 0, 0 },
		};
		// In front of a 3D layer this layer becomes its front face: light it turned like the object.
		const int extrude = Extrude3DIndex(obj);
		if (extrude >= 0 && &l < &obj.stack[extrude]) {
			const Layer& x = obj.stack[extrude];
			float turnX = 0, turnY = 0;
			TurnOf(x, curFrame_, curFps_, turnX, turnY);
			const M3 r = M3::RotY(x.rotY + turnY) * M3::RotX(x.rotX + turnX);
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j) c.rot[i][j] = r.m[i][j];
		}

		// Direct2D has queued work on kTmp (and kC): finish it before Direct3D reads it.
		Target(kB, false);
		hr = dc_->Flush();
		if (FAILED(hr)) return hr;
		const D3D11_VIEWPORT vp = { 0, 0, W, H, 0.0f, 1.0f };
		context_->RSSetViewports(1, &vp);
		context_->RSSetState(raster3d_.Get());
		context_->OMSetDepthStencilState(nullptr, 0);
		context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
		context_->IASetInputLayout(nullptr);
		context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context_->VSSetShader(vsSurf_.Get(), nullptr, 0);
		ID3D11Buffer* cb = cbSurf_.Get();
		context_->PSSetConstantBuffers(0, 1, &cb);
		context_->UpdateSubresource(cbSurf_.Get(), 0, nullptr, &c, 0, 0);

		int seed = 0;
		if (bevel) {
			FullPass(psSeed_.Get(), seedRtv_[0].Get(), slotSrv_[kTmp].Get(), nullptr);
			// Jump flood from the bevel width down to 1 px (farther pixels are flat anyway), then 1 px again.
			int step = 1;
			while (step < (int)std::ceil(widthPx) + 1) step <<= 1;
			std::vector<int> steps;
			for (int s = step; s >= 1; s >>= 1) steps.push_back(s);
			steps.push_back(1);
			for (int s : steps) {
				c.jump[0] = (float)s;
				context_->UpdateSubresource(cbSurf_.Get(), 0, nullptr, &c, 0, 0);
				FullPass(psJump_.Get(), seedRtv_[1 - seed].Get(), nullptr, seedSrv_[seed].Get());
				seed = 1 - seed;
			}
			// Bevel height into the other target: the shade pass reads it as t1.
			FullPass(psHeight_.Get(), seedRtv_[1 - seed].Get(), slotSrv_[kTmp].Get(), seedSrv_[seed].Get());
			seed = 1 - seed;
			for (int dir = 0; dir < 2; ++dir) {
				c.jump[1] = dir == 0 ? 1.0f : 0.0f;
				c.jump[2] = dir == 0 ? 0.0f : 1.0f;
				context_->UpdateSubresource(cbSurf_.Get(), 0, nullptr, &c, 0, 0);
				FullPass(psBlur_.Get(), seedRtv_[1 - seed].Get(), nullptr, seedSrv_[seed].Get());
				seed = 1 - seed;
			}
		}
		FullPass(psShade_.Get(), slotRtv_[kA].Get(), slotSrv_[kTmp].Get(), seedSrv_[seed].Get(), bump ? slotSrv_[kC].Get() : nullptr);
		ID3D11ShaderResourceView* none[3] = { nullptr, nullptr, nullptr };
		context_->PSSetShaderResources(0, 3, none);
		context_->OMSetRenderTargets(0, nullptr, nullptr);

		Target(kTmp, true);
		dc_->DrawImage(Slot(kA));
		return S_OK;
	}

	// ------------------------------------------------------------ 3D layer
	struct SceneConstants {
		float view[4];   // output width, height, camera distance, -
		float depthK[4]; // clip z = depthK.x * w + depthK.y
		float light[4];  // towards the light, intensity
		float camera[4]; // camera position, ambient
		float side1[4];  // linear colour, metallic
		float side2[4];  // linear colour, roughness
	};

	bool Init3D()
	{
		if (ready3D_) return true;
		if (failed3D_) return false;
		failed3D_ = true; // until everything below worked
		if (FAILED(device_->CreateVertexShader(g_vs3d, sizeof(g_vs3d), nullptr, &vs3d_))) return false;
		if (FAILED(device_->CreatePixelShader(g_ps3d, sizeof(g_ps3d), nullptr, &ps3d_))) return false;
		const D3D11_INPUT_ELEMENT_DESC layout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		if (FAILED(device_->CreateInputLayout(layout, ARRAYSIZE(layout), g_vs3d, sizeof(g_vs3d), &layout3d_))) return false;
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = sizeof(SceneConstants);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		if (FAILED(device_->CreateBuffer(&bd, nullptr, &cb3d_))) return false;
		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(device_->CreateSamplerState(&sd, &sampler3d_))) return false;
		D3D11_RASTERIZER_DESC rd{};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.DepthClipEnable = TRUE;
		rd.MultisampleEnable = TRUE;
		if (FAILED(device_->CreateRasterizerState(&rd, &raster3d_))) return false;
		D3D11_DEPTH_STENCIL_DESC dd{};
		dd.DepthEnable = TRUE;
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		if (FAILED(device_->CreateDepthStencilState(&dd, &depthState3d_))) return false;
		if (FAILED(device_->CreateVertexShader(g_vsSurf, sizeof(g_vsSurf), nullptr, &vsSurf_))) return false;
		if (FAILED(device_->CreatePixelShader(g_psSeed, sizeof(g_psSeed), nullptr, &psSeed_))) return false;
		if (FAILED(device_->CreatePixelShader(g_psJump, sizeof(g_psJump), nullptr, &psJump_))) return false;
		if (FAILED(device_->CreatePixelShader(g_psHeight, sizeof(g_psHeight), nullptr, &psHeight_))) return false;
		if (FAILED(device_->CreatePixelShader(g_psBlur, sizeof(g_psBlur), nullptr, &psBlur_))) return false;
		if (FAILED(device_->CreatePixelShader(g_psShade, sizeof(g_psShade), nullptr, &psShade_))) return false;
		bd.ByteWidth = sizeof(SurfaceConstants);
		if (FAILED(device_->CreateBuffer(&bd, nullptr, &cbSurf_))) return false;
		samples3d_ = 1;
		for (UINT n : { 8u, 4u, 2u }) {
			UINT qc = 0, qd = 0;
			if (SUCCEEDED(device_->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, n, &qc)) && qc > 0
				&& SUCCEEDED(device_->CheckMultisampleQualityLevels(DXGI_FORMAT_D32_FLOAT, n, &qd)) && qd > 0) {
				samples3d_ = n;
				break;
			}
		}
		failed3D_ = false;
		ready3D_ = true;
		return true;
	}

	void Reset3D()
	{
		vs3d_.Reset(); ps3d_.Reset(); layout3d_.Reset(); cb3d_.Reset(); sampler3d_.Reset();
		raster3d_.Reset(); depthState3d_.Reset(); vb3d_.Reset();
		vsSurf_.Reset(); psSeed_.Reset(); psJump_.Reset(); psHeight_.Reset(); psBlur_.Reset(); psShade_.Reset(); cbSurf_.Reset();
		ms3d_.Reset(); rtv3d_.Reset(); depth3d_.Reset(); dsv3d_.Reset(); resolve3d_.Reset(); bitmap3d_.Reset();
		vbBytes3d_ = 0;
		w3d_ = h3d_ = 0;
		ready3D_ = failed3D_ = false;
	}

	// Multisampled colour + depth target of at least w x h (grown, never shrunk), resolved into a
	// texture Direct2D can draw.
	HRESULT Ensure3DTarget(int w, int h)
	{
		if (ms3d_ && w <= w3d_ && h <= h3d_) return S_OK;
		w = std::max((w + 127) / 128 * 128, w3d_);
		h = std::max((h + 127) / 128 * 128, h3d_);
		ms3d_.Reset(); rtv3d_.Reset(); depth3d_.Reset(); dsv3d_.Reset(); resolve3d_.Reset(); bitmap3d_.Reset();
		w3d_ = h3d_ = 0;
		D3D11_TEXTURE2D_DESC td{};
		td.Width = (UINT)w;
		td.Height = (UINT)h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		td.SampleDesc.Count = samples3d_;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET;
		HRESULT hr = device_->CreateTexture2D(&td, nullptr, &ms3d_);
		if (FAILED(hr)) return hr;
		hr = device_->CreateRenderTargetView(ms3d_.Get(), nullptr, &rtv3d_);
		if (FAILED(hr)) return hr;
		td.Format = DXGI_FORMAT_D32_FLOAT;
		td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		hr = device_->CreateTexture2D(&td, nullptr, &depth3d_);
		if (FAILED(hr)) return hr;
		hr = device_->CreateDepthStencilView(depth3d_.Get(), nullptr, &dsv3d_);
		if (FAILED(hr)) return hr;
		td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		hr = device_->CreateTexture2D(&td, nullptr, &resolve3d_);
		if (FAILED(hr)) return hr;
		ComPtr<IDXGISurface> s;
		hr = resolve3d_.As(&s);
		if (FAILED(hr)) return hr;
		const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
			D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
		hr = dc_->CreateBitmapFromDxgiSurface(s.Get(), &bp, &bitmap3d_);
		if (FAILED(hr)) return hr;
		w3d_ = w;
		h3d_ = h;
		return S_OK;
	}

	// Clean outline of one run as the layers in front of the 3D layer cover it (glyph grown by the
	// widest outer stroke), flattened to polygons.
	HRESULT Silhouette(ID2D1Geometry* g, float grow, ID2D1StrokeStyle* style, float tol, std::vector<Poly>& out, ComPtr<ID2D1PathGeometry>& clean)
	{
		HRESULT hr = factory_->CreatePathGeometry(&clean);
		if (FAILED(hr)) return hr;
		ComPtr<ID2D1GeometrySink> sink;
		hr = clean->Open(&sink);
		if (FAILED(hr)) return hr;
		if (grow > 0) {
			ComPtr<ID2D1PathGeometry> wide;
			hr = factory_->CreatePathGeometry(&wide);
			if (FAILED(hr)) return hr;
			ComPtr<ID2D1GeometrySink> ws;
			hr = wide->Open(&ws);
			if (FAILED(hr)) return hr;
			hr = g->Widen(grow * 2, style, nullptr, tol, ws.Get());
			if (FAILED(hr)) return hr;
			hr = ws->Close();
			if (FAILED(hr)) return hr;
			hr = g->CombineWithGeometry(wide.Get(), D2D1_COMBINE_MODE_UNION, nullptr, tol, sink.Get());
		} else {
			hr = g->Outline(nullptr, tol, sink.Get());
		}
		if (FAILED(hr)) return hr;
		hr = sink->Close();
		if (FAILED(hr)) return hr;
		PolySink polys;
		hr = clean->Simplify(D2D1_GEOMETRY_SIMPLIFICATION_OPTION_LINES, nullptr, tol, &polys);
		if (FAILED(hr)) return hr;
		const float eps = tol * 0.05f;
		for (Poly& f : polys.figures) {
			Poly p;
			for (const D2D1_POINT_2F& q : f)
				if (p.empty() || std::fabs(q.x - p.back().x) > eps || std::fabs(q.y - p.back().y) > eps) p.push_back(q);
			while (p.size() > 1 && std::fabs(p.front().x - p.back().x) <= eps && std::fabs(p.front().y - p.back().y) <= eps) p.pop_back();
			if (p.size() >= 3 && std::fabs(SignedArea(p)) > tol * tol) out.push_back(std::move(p));
		}
		return S_OK;
	}

	// Triangles of a filled shape (winding fill).
	HRESULT Tessellate(ID2D1Geometry* g, float tol, std::vector<D2D1_TRIANGLE>& out)
	{
		TriSink tris;
		HRESULT hr = g->Tessellate(nullptr, tol, &tris);
		if (FAILED(hr)) return hr;
		out = std::move(tris.triangles);
		return S_OK;
	}

	HRESULT PolysToGeometry(const std::vector<Poly>& polys, ComPtr<ID2D1PathGeometry>& out)
	{
		HRESULT hr = factory_->CreatePathGeometry(&out);
		if (FAILED(hr)) return hr;
		ComPtr<ID2D1GeometrySink> sink;
		hr = out->Open(&sink);
		if (FAILED(hr)) return hr;
		sink->SetFillMode(D2D1_FILL_MODE_WINDING);
		for (const Poly& p : polys) {
			sink->BeginFigure(p[0], D2D1_FIGURE_BEGIN_FILLED);
			sink->AddLines(p.data() + 1, (UINT32)p.size() - 1);
			sink->EndFigure(D2D1_FIGURE_END_CLOSED);
		}
		return sink->Close();
	}

	// Mesh of one run at rest, layout space.
	HRESULT BuildRunMesh(const Run& run, float grow, ID2D1StrokeStyle* style, float tol, const std::vector<BevelStep>& prof,
		float bevelH, float depth, std::vector<Vertex3D>& local)
	{
		std::vector<Poly> figs;
		ComPtr<ID2D1PathGeometry> clean;
		HRESULT hr = Silhouette(run.geometry.Get(), grow, style, tol, figs, clean);
		if (FAILED(hr)) return hr;
		if (figs.empty()) return S_OK;

		std::vector<Poly> frontOutline;
		ExtrudeFigures(figs, prof, bevelH, depth, local, frontOutline);
		auto addCap = [&](ID2D1Geometry* g, float z, float kind, float nz) -> HRESULT {
			std::vector<D2D1_TRIANGLE> tris;
			HRESULT h2 = Tessellate(g, tol, tris);
			if (FAILED(h2)) return h2;
			for (const D2D1_TRIANGLE& t : tris)
				for (const D2D1_POINT_2F& q : { t.point1, t.point2, t.point3 })
					local.push_back({ q.x, q.y, z, 0, 0, nz, 0, 0, kind, kind > 2 ? 1.0f : 0.0f });
			return S_OK;
		};
		ComPtr<ID2D1PathGeometry> front;
		hr = PolysToGeometry(frontOutline, front);
		if (FAILED(hr)) return hr;
		hr = addCap(front.Get(), 0, 0, -1);
		if (FAILED(hr)) return hr;
		if (depth > 0) hr = addCap(clean.Get(), depth, 3, 1);
		return hr;
	}

	// Draws the 3D layer into kTmp (cleared). kAccum holds the layers in front of it: their combined
	// shape is extruded and their look is the front face.
	HRESULT Render3D(const TextObject& obj, const Layer& l, int index, const TextShape& shape)
	{
		Target(kTmp, true);
		if (!shape.all || shape.runs.empty()) return S_OK;
		if (!Init3D()) {
			dc_->DrawImage(Slot(kAccum)); // no 3D on this device: keep the 2D look
			return S_OK;
		}

		// Outline grows by the widest outer / centre stroke in front.
		float grow = 0;
		ComPtr<ID2D1StrokeStyle> style;
		for (int i = 0; i < index; ++i) {
			const Layer& f = obj.stack[i];
			if (!f.enabled || f.opacity <= 0 || f.type != LayerType::Stroke || f.width <= 0) continue;
			const float g = f.position == StrokePosition::Outer ? f.width : f.position == StrokePosition::Center ? f.width * 0.5f : 0;
			if (g > grow) { grow = g; style = StrokeStyle(f); }
		}

		const float ds = shape.deviceScale;
		const float tol = std::clamp(0.3f / std::max(ds, 0.01f), 0.02f, 2.0f);
		const float depth = std::max(l.depth, 0.0f);
		float bevelW = 0, bevelH = 0;
		const std::vector<BevelStep> prof = BevelProfile(l, depth, bevelW, bevelH);
		const D2D1_MATRIX_3X2_F& A = shape.toOutput;
		const float W = (float)targetW_, H = (float)targetH_;
		float turnX = 0, turnY = 0;
		TurnOf(l, curFrame_, curFps_, turnX, turnY);
		const M3 objRot = M3::RotY(l.rotY + turnY) * M3::RotX(l.rotX + turnX);
		const float objPivotZ = depth * ds * 0.5f;

		// The mesh at rest only depends on the object and the output scale: cache it across frames.
		// Only what changes the shape goes into the key: turning, lighting, colours, placement and
		// animation are applied per frame.
		Doc keyDoc;
		keyDoc.objects.push_back(obj);
		{
			TextObject& k = keyDoc.objects[0];
			const Layer defaults;
			Layer& x = k.stack[index];
			x.rotX = x.rotY = 0;
			x.turn = TurnMode::None;
			x.turnX = defaults.turnX; x.turnSpeed = defaults.turnSpeed; x.turnAmount = defaults.turnAmount;
			x.turnPeriod = defaults.turnPeriod; x.turnPhase = defaults.turnPhase;
			x.perspective = defaults.perspective;
			x.metallic = defaults.metallic; x.roughness = defaults.roughness;
			for (int i = 0; i < index; ++i) { // only stroke widths / joins shape the outline
				Layer& f = k.stack[i];
				f.paint = Paint();
				f.material = Material::None;
				f.bevel = BevelShape::None;
				f.metallic = defaults.metallic; f.roughness = defaults.roughness; f.brushed = false;
				f.bump = Texture();
				f.tex = Texture();
			}
			k.light = Light();
			x.sideColor = defaults.sideColor; x.sideColor2 = defaults.sideColor2;
			x.opacity = 100;
			k.stack.resize(index + 1);
			k.x = k.y = 0;
			k.rotation = 0;
			k.opacity = 100;
			k.animIn = k.animOut = AnimSpec();
		}
		char extra[64];
		snprintf(extra, sizeof(extra), "|%d|%.5g", index, ds);
		const std::string key = DocToJson(keyDoc).dump(false) + extra;
		MeshCache* cache = nullptr;
		for (auto& c : meshCache_) if (c.key == key) cache = &c;
		if (!cache) {
			if (meshCache_.size() >= 16) meshCache_.erase(meshCache_.begin());
			meshCache_.push_back({ key, {} });
			cache = &meshCache_.back();
		}

		std::vector<Vertex3D> verts;
		for (const Run& run : shape.runs) {
			const RunMesh* found = nullptr;
			for (const RunMesh& rm : cache->runs) if (rm.ch == run.ch && rm.ruby == run.ruby) found = &rm;
			if (!found) {
				RunMesh rm{ run.ch, run.ruby, {} };
				HRESULT hr = BuildRunMesh(run, grow, style.Get(), tol, prof, bevelH, depth, rm.local);
				if (FAILED(hr)) return hr;
				cache->runs.push_back(std::move(rm));
				found = &cache->runs.back();
			}
			const std::vector<Vertex3D>& local = found->local;
			if (local.empty()) continue;

			// layout -> animated (m, depth scaled with it) -> unit turn -> output pixels -> object turn
			const D2D1_MATRIX_3X2_F& m = run.m;
			const float k = std::sqrt(std::fabs(m._11 * m._22 - m._12 * m._21));
			const M3 unitRot = M3::RotY(run.rotY) * M3::RotX(run.rotX);
			const float pz = depth * k * 0.5f;
			const M3 lin = objRot * M3::From2D(A, ds) * unitRot * M3::From2D(m, k);
			const M3 nrm = lin.NormalMatrix();
			const float growK = run.depth;
			const float dzOut = run.dz * ds;
			for (Vertex3D v : local) {
				// The extrusion grows from the bevel's back edge: the front face and the bevel stay.
				const float vz = (growK != 1.0f && v.z > bevelH) ? bevelH + (v.z - bevelH) * growK : v.z;
				float x = v.x * m._11 + v.y * m._21 + m._31, y = v.x * m._12 + v.y * m._22 + m._32, z = vz * k;
				// The front face shows the flat 2D render at the same (unturned) spot.
				v.u = (x * A._11 + y * A._21 + A._31) / W;
				v.v = (x * A._12 + y * A._22 + A._32) / H;
				x -= run.pivot.x; y -= run.pivot.y; z -= pz;
				unitRot.Apply(x, y, z);
				x += run.pivot.x; y += run.pivot.y; z += pz;
				const float ox = x * A._11 + y * A._21 + A._31, oy = x * A._12 + y * A._22 + A._32;
				x = ox - shape.anchorOut.x; y = oy - shape.anchorOut.y; z = z * ds - objPivotZ;
				objRot.Apply(x, y, z);
				v.x = x + shape.anchorOut.x; v.y = y + shape.anchorOut.y; v.z = z + objPivotZ + dzOut;
				nrm.Apply(v.nx, v.ny, v.nz);
				verts.push_back(v);
			}
		}
		if (verts.empty()) return S_OK;

		// Camera: about a 50 mm lens at perspective 50, nearly orthographic at 0.
		const float D = W * 1.39f * std::pow(2.0f, (50.0f - std::clamp(l.perspective, 0.0f, 100.0f)) / 25.0f);
		float wMin = 1e30f, wMax = -1e30f;
		float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
		for (const Vertex3D& v : verts) {
			const float w = std::max((D + v.z) / D, 0.02f);
			wMin = std::min(wMin, w);
			wMax = std::max(wMax, w);
			const float sx = W * 0.5f + (v.x - W * 0.5f) / w, sy = H * 0.5f + (v.y - H * 0.5f) / w;
			bx0 = std::min(bx0, sx); by0 = std::min(by0, sy); bx1 = std::max(bx1, sx); by1 = std::max(by1, sy);
		}
		const int left = std::max((int)std::floor(bx0) - 2, 0), top = std::max((int)std::floor(by0) - 2, 0);
		const int right = std::min((int)std::ceil(bx1) + 2, (int)W), bottom = std::min((int)std::ceil(by1) + 2, (int)H);
		if (right <= left || bottom <= top) return S_OK;
		wMin *= 0.999f;
		wMax = std::max(wMax * 1.001f, wMin * 1.001f);

		HRESULT hr = Ensure3DTarget(right - left, bottom - top);
		if (FAILED(hr)) return hr;
		const UINT bytes = (UINT)(verts.size() * sizeof(Vertex3D));
		if (bytes > vbBytes3d_) {
			vb3d_.Reset();
			D3D11_BUFFER_DESC bd{};
			bd.ByteWidth = std::max(bytes * 3 / 2, 1u << 16);
			bd.Usage = D3D11_USAGE_DYNAMIC;
			bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
			bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			hr = device_->CreateBuffer(&bd, nullptr, &vb3d_);
			if (FAILED(hr)) return hr;
			vbBytes3d_ = bd.ByteWidth;
		}
		D3D11_MAPPED_SUBRESOURCE mapped{};
		hr = context_->Map(vb3d_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(hr)) return hr;
		memcpy(mapped.pData, verts.data(), bytes);
		context_->Unmap(vb3d_.Get(), 0);

		auto lin = [](uint32_t argb, int shift) { return std::pow(((argb >> shift) & 0xFF) / 255.0f, 2.2f); };
		const float4v ld = LightVector(curLight_);
		const float zDen = 1.0f / wMin - 1.0f / wMax;
		SceneConstants c = {
			{ W, H, D, curLight_.env == EnvStyle::Studio ? 1.0f : 0.0f },
			{ 1.0f / (wMin * zDen), -1.0f / zDen, 0, 0 },
			{ ld.v[0], ld.v[1], ld.v[2], ld.v[3] },
			{ W * 0.5f, H * 0.5f, -D, std::max(curLight_.ambient, 0.0f) / 100.0f },
			{ lin(l.sideColor, 16), lin(l.sideColor, 8), lin(l.sideColor, 0), std::clamp(l.metallic / 100.0f, 0.0f, 1.0f) },
			{ lin(l.sideColor2, 16), lin(l.sideColor2, 8), lin(l.sideColor2, 0), std::clamp(l.roughness / 100.0f, 0.0f, 1.0f) },
		};
		context_->UpdateSubresource(cb3d_.Get(), 0, nullptr, &c, 0, 0);

		// Direct2D has queued work on kAccum: finish it before Direct3D reads it.
		hr = dc_->Flush();
		if (FAILED(hr)) return hr;
		const float clear[4] = { 0, 0, 0, 0 };
		context_->ClearRenderTargetView(rtv3d_.Get(), clear);
		context_->ClearDepthStencilView(dsv3d_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
		ID3D11RenderTargetView* rtv = rtv3d_.Get();
		context_->OMSetRenderTargets(1, &rtv, dsv3d_.Get());
		context_->OMSetDepthStencilState(depthState3d_.Get(), 0);
		context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
		// The target only covers the text's box: shift the full-frame viewport so pixels line up.
		const D3D11_VIEWPORT vp = { (float)-left, (float)-top, W, H, 0.0f, 1.0f };
		context_->RSSetViewports(1, &vp);
		context_->RSSetState(raster3d_.Get());
		const UINT stride = sizeof(Vertex3D), offset = 0;
		ID3D11Buffer* vb = vb3d_.Get();
		context_->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
		context_->IASetInputLayout(layout3d_.Get());
		context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context_->VSSetShader(vs3d_.Get(), nullptr, 0);
		ID3D11Buffer* cb = cb3d_.Get();
		context_->VSSetConstantBuffers(0, 1, &cb);
		context_->PSSetShader(ps3d_.Get(), nullptr, 0);
		context_->PSSetConstantBuffers(0, 1, &cb);
		ID3D11ShaderResourceView* srv = slotSrv_[kAccum].Get();
		context_->PSSetShaderResources(0, 1, &srv);
		ID3D11SamplerState* smp = sampler3d_.Get();
		context_->PSSetSamplers(0, 1, &smp);
		context_->Draw((UINT)verts.size(), 0);
		ID3D11ShaderResourceView* noSrv = nullptr;
		context_->PSSetShaderResources(0, 1, &noSrv);
		context_->OMSetRenderTargets(0, nullptr, nullptr);
		context_->ResolveSubresource(resolve3d_.Get(), 0, ms3d_.Get(), 0, DXGI_FORMAT_B8G8R8A8_UNORM);

		const D2D1_RECT_F src = D2D1::RectF(0, 0, (float)(right - left), (float)(bottom - top));
		dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
			D2D1::IdentityMatrix(), std::clamp(l.opacity / 100.0f, 0.0f, 1.0f)), nullptr);
		const D2D1_POINT_2F at = D2D1::Point2F((float)left, (float)top);
		dc_->DrawImage(bitmap3d_.Get(), &at, &src, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
		dc_->PopLayer();
		return S_OK;
	}

	HRESULT RenderObject(const TextObject& obj, int frame, int durationFrames, float outScale)
	{
		const float opacity = ObjectOpacity(obj);
		if (!obj.visible || opacity <= 0.0f) return S_OK;
		curLight_ = AnimatedLight(obj.light, frame, curFps_);

		TextShape shape;
		HRESULT hr = BuildShape(obj, outScale, shape, frame, durationFrames);
		if (FAILED(hr)) return hr;
		if (!HasAnimation(obj)) return RenderStack(obj, shape, opacity);

		// Characters at different opacities each get their own pass of the whole stack, so a fading
		// character keeps its own stroke / shadow. Fainter groups go below; the most opaque group is on
		// top and also carries the background.
		std::vector<int> levels;
		for (const Run& r : shape.runs) levels.push_back((int)std::lround(r.opacity * 64));
		std::vector<int> order = levels;
		std::sort(order.begin(), order.end());
		order.erase(std::unique(order.begin(), order.end()), order.end());
		order.erase(std::remove(order.begin(), order.end(), 0), order.end());
		for (size_t g = 0; g < order.size(); ++g) {
			TextShape part = shape;
			part.runs.clear();
			for (size_t i = 0; i < shape.runs.size(); ++i)
				if (levels[i] == order[g]) part.runs.push_back(shape.runs[i]);
			part.background = g + 1 == order.size();
			hr = MakeUnion(part);
			if (FAILED(hr)) return hr;
			hr = RenderStack(obj, part, opacity * order[g] / 64.0f);
			if (FAILED(hr)) return hr;
		}
		return S_OK;
	}

	// Renders the appearance stack of `shape` and composites it onto the frame.
	HRESULT RenderStack(const TextObject& obj, const TextShape& shape, float opacity)
	{
		HRESULT hr = S_OK;
		Target(kAccum, true);
		const int extrude = Extrude3DIndex(obj);
		for (size_t i = 0; i < obj.stack.size(); ++i) {
			const Layer& l = obj.stack[i];
			if (!l.enabled || l.opacity <= 0) continue;
			if (l.type == LayerType::Extrude) {
				if ((int)i != extrude) continue; // only the first 3D layer
				hr = Render3D(obj, l, (int)i, shape);
				if (FAILED(hr)) return hr;
				// The 3D render already carries the front layers on its front face: it replaces them.
				Target(kAccum, true);
				dc_->DrawImage(Slot(kTmp));
				continue;
			}
			if (l.type == LayerType::Texture) {
				// Laid over what is in front of it, in place (not behind, like the other layers).
				hr = LayTexture(l.tex, shape, kAccum, l.clip ? kAccum : -1, std::clamp(l.opacity / 100.0f, 0.0f, 1.0f) * l.tex.opacity / 100.0f);
				if (FAILED(hr)) return hr;
				continue;
			}
			hr = RenderLayer(obj, l, shape);
			if (FAILED(hr)) return hr;
			if (HasSurface(l)) {
				hr = ApplySurface(obj, l, shape);
				if (FAILED(hr)) return hr;
			}
			// Each further layer goes behind everything drawn so far.
			Target(kAccum, false);
			dc_->DrawImage(Slot(kTmp), Origin(), D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_DESTINATION_OVER);
		}

		Target(kFrame, false);
		dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
			D2D1::IdentityMatrix(), opacity), nullptr);
		dc_->DrawImage(Slot(kAccum));
		dc_->PopLayer();
		return S_OK;
	}

	HRESULT RenderLocked(const Doc& doc, int frame, int outW, int outH, uint8_t* dst, int dstRowBytes, bool bottomUp)
	{
		if (outW <= 0 || outH <= 0) return E_INVALIDARG;
		HRESULT hr = EnsureTargets(outW, outH);
		if (FAILED(hr)) return hr;

		const float outScale = (float)outW / (float)doc.width;
		outScale_ = outScale;
		docW_ = doc.width;
		docH_ = doc.height;
		curFrame_ = frame;
		curFps_ = (float)doc.vidScale / (float)std::max(doc.vidSampleSize, 1);
		dc_->BeginDraw();
		dc_->SetTransform(D2D1::IdentityMatrix());
		Target(kFrame, true);
		HRESULT drawHr = S_OK;
		for (const TextObject& obj : doc.objects) {
			drawHr = RenderObject(obj, frame, doc.durationFrames, outScale);
			if (FAILED(drawHr)) break;
		}
		hr = dc_->EndDraw();
		dc_->SetTarget(nullptr);
		if (FAILED(hr)) return hr;
		if (FAILED(drawHr)) return drawHr;

		context_->CopyResource(staging_.Get(), frameTexture_.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
		if (FAILED(hr)) return hr;

		for (int y = 0; y < outH; ++y) {
			const uint8_t* s = static_cast<const uint8_t*>(mapped.pData) + (size_t)y * mapped.RowPitch;
			uint8_t* d = dst + (size_t)(bottomUp ? (outH - 1 - y) : y) * dstRowBytes;
			for (int x = 0; x < outW; ++x, s += 4, d += 4) {
				const uint32_t a = s[3];
				if (a == 0) {
					d[0] = d[1] = d[2] = d[3] = 0;
				} else if (a == 255) {
					d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
				} else {
					// premultiplied -> straight
					for (int c = 0; c < 3; ++c) {
						uint32_t v = (s[c] * 255u + a / 2) / a;
						d[c] = (uint8_t)(v > 255 ? 255 : v);
					}
					d[3] = (uint8_t)a;
				}
			}
		}
		context_->Unmap(staging_.Get(), 0);
		return S_OK;
	}

	std::mutex mutex_;
	ComPtr<ID3D11Device> device_;
	ComPtr<ID3D11DeviceContext> context_;
	ComPtr<ID2D1Factory1> factory_;
	ComPtr<ID2D1Device> d2dDevice_;
	ComPtr<ID2D1DeviceContext> dc_;
	ComPtr<IDWriteFactory> dwrite_;
	ComPtr<IDWriteTextAnalyzer2> analyzer2_;
	ComPtr<ID3D11Texture2D> frameTexture_, staging_;
	ComPtr<ID2D1Bitmap1> slots_[kSlotCount];
	ComPtr<ID3D11Texture2D> slotTex_[kSlotCount];
	ComPtr<ID3D11ShaderResourceView> slotSrv_[kSlotCount]; // scratch slots for Direct3D passes
	ComPtr<ID3D11RenderTargetView> slotRtv_[kSlotCount];
	// surface pass
	ComPtr<ID3D11VertexShader> vsSurf_;
	ComPtr<ID3D11PixelShader> psSeed_, psJump_, psHeight_, psBlur_, psShade_;
	ComPtr<ID3D11Buffer> cbSurf_;
	ComPtr<ID3D11Texture2D> seedTex_[2];
	ComPtr<ID3D11ShaderResourceView> seedSrv_[2];
	ComPtr<ID3D11RenderTargetView> seedRtv_[2];
	int targetW_ = 0, targetH_ = 0;
	// 3D layer
	struct RunMesh {
		int ch;
		bool ruby;
		std::vector<Vertex3D> local;
	};
	struct MeshCache {
		std::string key;
		std::vector<RunMesh> runs;
	};
	std::vector<MeshCache> meshCache_; // oldest first
	std::vector<std::pair<std::wstring, std::vector<float>>> opticalCache_; // optical kerning, oldest first
	std::vector<TexSlot> texCache_;    // texture frames as Direct2D bitmaps, oldest first
	// state of the frame being rendered
	Light curLight_;
	int curFrame_ = 0;
	float curFps_ = 30;
	int docW_ = 1920, docH_ = 1080;
	float outScale_ = 1;
	bool ready3D_ = false, failed3D_ = false;
	UINT samples3d_ = 1;
	ComPtr<ID3D11VertexShader> vs3d_;
	ComPtr<ID3D11PixelShader> ps3d_;
	ComPtr<ID3D11InputLayout> layout3d_;
	ComPtr<ID3D11Buffer> cb3d_, vb3d_;
	UINT vbBytes3d_ = 0;
	ComPtr<ID3D11SamplerState> sampler3d_;
	ComPtr<ID3D11RasterizerState> raster3d_;
	ComPtr<ID3D11DepthStencilState> depthState3d_;
	ComPtr<ID3D11Texture2D> ms3d_, depth3d_, resolve3d_;
	ComPtr<ID3D11RenderTargetView> rtv3d_;
	ComPtr<ID3D11DepthStencilView> dsv3d_;
	ComPtr<ID2D1Bitmap1> bitmap3d_;
	int w3d_ = 0, h3d_ = 0;
	std::wstring description_;
};

} // namespace

bool RenderFrame(const Doc& doc, int frame, int outW, int outH, uint8_t* dst, int dstRowBytes, bool bottomUp)
{
	return Engine::Get().Render(doc, frame, outW, outH, dst, dstRowBytes, bottomUp);
}

bool GetObjectQuad(const Doc& doc, size_t objectIndex, float outQuad[8])
{
	return Engine::Get().Quad(doc, objectIndex, outQuad);
}

bool GetObjectCells(const Doc& doc, size_t objectIndex, std::vector<CharCell>& out)
{
	return Engine::Get().Cells(doc, objectIndex, out);
}

std::wstring RendererDescription()
{
	return Engine::Get().Description();
}

} // namespace tdp
