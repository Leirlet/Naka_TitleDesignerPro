// Renders one frame of a .tdp to a BMP (composited over a checkerboard) for checking the renderer
// outside Premiere.  usage: tdp_render_test <in.tdp> <out.bmp> [frame] [width]

#include <windows.h>

#include "tdp_doc.h"
#include "tdp_media.h"
#include "tdp_render.h"

#include <cstdio>
#include <vector>

int wmain(int argc, wchar_t** argv)
{
	// tdp_render_test --prepare <texture file>: makes the frame cache of a video (what the designer does
	// when a texture is chosen) and prints what it found.
	if (argc >= 3 && wcscmp(argv[1], L"--prepare") == 0) {
		const tdp::media::Info info = tdp::media::Prepare(argv[2], [](int n) { wprintf(L"\r%d frames", n); });
		wprintf(L"\n%s: %s %dx%d, %d frames, %.3f fps %s\n", argv[2], info.ok ? L"ok" : L"NOT READY", info.w, info.h, info.frames, info.fps,
			info.error.c_str());
		return info.ok ? 0 : 1;
	}
	if (argc < 3) {
		fwprintf(stderr, L"usage: tdp_render_test <in.tdp> <out.bmp> [frame] [width]\n       tdp_render_test --prepare <texture file>\n");
		return 2;
	}
	tdp::Doc doc;
	if (!tdp::LoadDoc(argv[1], doc)) {
		fwprintf(stderr, L"cannot read %s\n", argv[1]);
		return 1;
	}
	const int frame = argc > 3 ? _wtoi(argv[3]) : doc.durationFrames - 1;
	const int w = argc > 4 ? _wtoi(argv[4]) : doc.width;
	const int h = (int)((long long)w * doc.height / doc.width);

	// Render bottom-up exactly like the importer does for Premiere.
	std::vector<uint8_t> px((size_t)w * h * 4);
	if (!tdp::RenderFrame(doc, frame, w, h, px.data(), w * 4, true)) {
		fwprintf(stderr, L"render failed\n");
		return 1;
	}
	wprintf(L"%s\n", tdp::RendererDescription().c_str());

	// Timing: average of 20 renders after the first (device / targets already created).
	LARGE_INTEGER freq, t0, t1;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);
	for (int i = 0; i < 20; ++i) tdp::RenderFrame(doc, frame, w, h, px.data(), w * 4, true);
	QueryPerformanceCounter(&t1);
	wprintf(L"%dx%d: %.2f ms/frame\n", w, h, (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart / 20);

	// Composite over a checkerboard so the alpha is visible.
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			uint8_t* p = &px[((size_t)y * w + x) * 4];
			const uint32_t bg = (((x / 32) + (y / 32)) & 1) ? 160 : 96;
			const uint32_t a = p[3];
			for (int c = 0; c < 3; ++c) p[c] = (uint8_t)((p[c] * a + bg * (255 - a) + 127) / 255);
			p[3] = 255;
		}

	BITMAPFILEHEADER fh{};
	BITMAPINFOHEADER ih{};
	ih.biSize = sizeof(ih);
	ih.biWidth = w;
	ih.biHeight = h; // bottom-up
	ih.biPlanes = 1;
	ih.biBitCount = 32;
	fh.bfType = 0x4D42;
	fh.bfOffBits = sizeof(fh) + sizeof(ih);
	fh.bfSize = fh.bfOffBits + (DWORD)px.size();
	FILE* f = nullptr;
	if (_wfopen_s(&f, argv[2], L"wb") != 0 || !f) return 1;
	fwrite(&fh, sizeof(fh), 1, f);
	fwrite(&ih, sizeof(ih), 1, f);
	fwrite(px.data(), 1, px.size(), f);
	fclose(f);
	return 0;
}
