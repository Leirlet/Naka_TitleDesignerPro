// NAKA Title Designer Pro - shared renderer (Direct3D 11 + Direct2D + DirectWrite)
#pragma once

#include "tdp_doc.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tdp {

// Renders one frame of the title at outW x outH (scaled from the doc resolution).
// dst receives straight-alpha BGRA 8bit. bottomUp = true writes row 0 at the bottom
// (Premiere's PPix layout); false writes top-down.
// Thread-safe (serialized internally).
bool RenderFrame(const Doc& doc, int frame, int outW, int outH,
	uint8_t* dst, int dstRowBytes, bool bottomUp);

// Corners of a text object's layout box in doc pixels (TL, TR, BR, BL as x,y pairs).
bool GetObjectQuad(const Doc& doc, size_t objectIndex, float outQuad[8]);

// The character cells (advance x ascent..descent) of a text object at rest, in doc pixels, one per
// glyph cluster in layout order: ch = first UTF-16 index, quad = TL, TR, BR, BL. Ruby is left out.
// The designer draws kerning handles between them.
struct CharCell {
	int ch = 0;
	float quad[8] = {};
};
bool GetObjectCells(const Doc& doc, size_t objectIndex, std::vector<CharCell>& out);

// e.g. "GPU: NVIDIA GeForce ..." or "WARP (CPU): ...". Empty if the engine failed to start.
std::wstring RendererDescription();

} // namespace tdp
