// NAKA Title Designer Pro - shared renderer (Direct3D 11 + Direct2D + DirectWrite)
#pragma once

#include "tdp_doc.h"

#include <cstdint>
#include <string>

namespace tdp {

// Renders one frame of the title at outW x outH (scaled from the doc resolution).
// dst receives straight-alpha BGRA 8bit. bottomUp = true writes row 0 at the bottom
// (Premiere's PPix layout); false writes top-down.
// Thread-safe (serialized internally).
bool RenderFrame(const Doc& doc, int frame, int outW, int outH,
	uint8_t* dst, int dstRowBytes, bool bottomUp);

// Corners of a text object's layout box in doc pixels (TL, TR, BR, BL as x,y pairs).
bool GetObjectQuad(const Doc& doc, size_t objectIndex, float outQuad[8]);

// e.g. "GPU: NVIDIA GeForce ..." or "WARP (CPU): ...". Empty if the engine failed to start.
std::wstring RendererDescription();

} // namespace tdp
