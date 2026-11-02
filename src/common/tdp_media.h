// NAKA Title Designer Pro - texture sources (stills and videos with alpha)
//
// Stills are decoded with WIC. Videos have no decoder in Windows for ProRes 4444 and the like, so they
// are converted once by ffmpeg.exe into a cache folder of PNG frames (alpha kept); frames are then read
// from there by index. The cache folder name comes from the file's path, size and time, so a changed
// file is converted again and the importer finds what the designer made.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tdp {
namespace media {

struct Frame {
	int w = 0, h = 0;
	std::vector<uint8_t> bgra; // premultiplied alpha, top-down
};

struct Info {
	bool ok = false;           // can be drawn right now
	bool video = false;
	bool converting = false;   // a video whose frames are being made
	int w = 0, h = 0;
	int frames = 1;
	double fps = 0;
	std::wstring error;        // why not ok (ffmpeg missing, unreadable file, ...)
	std::wstring id;           // identifies the content (path, size, time): changes when the file does
};

// What the file is and whether it can be drawn. Never converts.
Info Lookup(const std::wstring& path);

// Like Lookup, but makes the frame cache of a video when it is missing (blocks; progress = frames made
// so far). Used by the designer when a file is chosen.
Info Prepare(const std::wstring& path, const std::function<void(int)>& progress = nullptr);

// Starts Prepare on a background thread (once per file and session). Used by the renderer when a video
// has not been converted yet.
void Request(const std::wstring& path);

// Frames converted so far of a video being converted (0 when none is).
int ConvertedFrames(const std::wstring& path);

// One frame (index in [0, frames)). Null if it cannot be read.
std::shared_ptr<const Frame> GetFrame(const std::wstring& path, int index);

// ffmpeg.exe: TDP_FFMPEG, next to the designer, PATH, C:\FFmpeg\bin. Empty if not found.
std::wstring FindFfmpeg();

} // namespace media
} // namespace tdp
