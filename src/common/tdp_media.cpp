#include "tdp_media.h"

#include <windows.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <list>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace tdp {
namespace media {

namespace {

struct Entry {
	Info info;
	std::wstring key;   // identifies the content (path, size, time): frames are cached under it
	std::wstring dir;   // video: folder of PNG frames
};

std::mutex g_mutex;
std::map<std::wstring, Entry> g_entries;     // ready textures, by lower-case path
std::set<std::wstring> g_converting;         // videos being converted right now
std::set<std::wstring> g_requested;          // videos a renderer already asked for
std::map<std::wstring, int> g_progress;      // frames made so far, by lower-case path
struct CachedFrame { std::wstring key; std::shared_ptr<const Frame> frame; };
std::list<CachedFrame> g_frames;             // most recent first
size_t g_frameBytes = 0;
constexpr size_t kFrameBudget = 320u << 20;

std::wstring Lower(std::wstring s)
{
	for (auto& c : s) c = (wchar_t)std::towlower(c);
	return s;
}

// ---- built-in noise textures: "builtin:grain" / "rough" / "cloud" / "fiber". 1024 x 1024, grey, opaque,
// average about 0.5 so that overlay / soft light leave the mean alone, and tileable.
constexpr int kNoiseSize = 1024;

bool IsBuiltin(const std::wstring& p) { return p.rfind(L"builtin:", 0) == 0; }

uint32_t Hash2(int x, int y, uint32_t seed)
{
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}
float Rnd(int x, int y, uint32_t seed) { return (Hash2(x, y, seed) & 0xFFFFFF) / 16777215.0f; }
float Smooth(float t) { return t * t * (3 - 2 * t); }

// Value noise on a lattice of cx x cy cells that wraps around the unit square.
float VNoise(float u, float v, int cx, int cy, uint32_t seed)
{
	const float x = u * cx, y = v * cy;
	const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
	const float fx = Smooth(x - x0), fy = Smooth(y - y0);
	auto at = [&](int i, int j) { return Rnd(((i % cx) + cx) % cx, ((j % cy) + cy) % cy, seed); };
	const float a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
	const float top = a + (b - a) * fx, bottom = c + (d - c) * fx;
	return top + (bottom - top) * fy;
}

float Fractal(float u, float v, int cx, int cy, int octaves, float gain, uint32_t seed)
{
	float sum = 0, amp = 1, norm = 0;
	for (int o = 0; o < octaves; ++o) {
		sum += amp * VNoise(u, v, cx << o, cy << o, seed + (uint32_t)o * 17u);
		norm += amp;
		amp *= gain;
	}
	return sum / norm;
}

std::shared_ptr<Frame> MakeNoise(const std::wstring& path)
{
	const std::wstring kind = path.substr(8);
	auto out = std::make_shared<Frame>();
	out->w = out->h = kNoiseSize;
	out->bgra.resize((size_t)kNoiseSize * kNoiseSize * 4);
	for (int y = 0; y < kNoiseSize; ++y)
		for (int x = 0; x < kNoiseSize; ++x) {
			const float u = (x + 0.5f) / kNoiseSize, v = (y + 0.5f) / kNoiseSize;
			const float grain = (Rnd(x, y, 1) + Rnd(x, y, 2)) * 0.5f; // triangular: fewer extremes than flat noise
			float n, contrast;
			if (kind == L"rough") { n = 0.85f * Fractal(u, v, 8, 8, 5, 0.55f, 11) + 0.15f * grain; contrast = 2.4f; }
			else if (kind == L"cloud") { n = Fractal(u, v, 3, 3, 4, 0.5f, 23); contrast = 2.2f; }
			else if (kind == L"fiber") { n = 0.9f * Fractal(u, v, 2, 256, 3, 0.6f, 31) + 0.1f * grain; contrast = 2.0f; } // streaks along x
			else { n = grain; contrast = 1.8f; }
			const float g = std::clamp(0.5f + (n - 0.5f) * contrast, 0.0f, 1.0f);
			uint8_t* p = &out->bgra[((size_t)y * kNoiseSize + x) * 4];
			p[0] = p[1] = p[2] = (uint8_t)(g * 255.0f + 0.5f);
			p[3] = 255;
		}
	return out;
}

bool IsStill(const std::wstring& path)
{
	const size_t dot = path.find_last_of(L'.');
	if (dot == std::wstring::npos) return false;
	const std::wstring e = Lower(path.substr(dot + 1));
	for (const wchar_t* s : { L"png", L"jpg", L"jpeg", L"jfif", L"bmp", L"tif", L"tiff", L"gif", L"webp", L"ico", L"dib" })
		if (e == s) return true;
	return false;
}

bool Stamp(const std::wstring& path, ULONGLONG& size, ULONGLONG& mtime)
{
	WIN32_FILE_ATTRIBUTE_DATA d{};
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
	size = ((ULONGLONG)d.nFileSizeHigh << 32) | d.nFileSizeLow;
	mtime = ((ULONGLONG)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
	return true;
}

std::wstring CacheRoot()
{
	PWSTR p = nullptr;
	std::wstring s;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &p))) s = p;
	CoTaskMemFree(p);
	return s + L"\\NAKA\\TitleDesignerPro\\cache";
}

std::wstring KeyFor(const std::wstring& path, ULONGLONG size, ULONGLONG mtime)
{
	// FNV-1a over the lower-case path, size and time.
	unsigned long long h = 1469598103934665603ull;
	auto mix = [&](unsigned long long v) { for (int i = 0; i < 8; ++i) { h ^= (v >> (i * 8)) & 0xFF; h *= 1099511628211ull; } };
	for (wchar_t c : Lower(path)) mix((unsigned long long)c);
	mix(size);
	mix(mtime);
	wchar_t buf[24];
	swprintf_s(buf, L"%016llx", h);
	return buf;
}

void EnsureCom()
{
	thread_local bool done = false;
	if (!done) {
		CoInitializeEx(nullptr, COINIT_MULTITHREADED); // RPC_E_CHANGED_MODE is fine: COM is usable either way
		done = true;
	}
}

ComPtr<IWICImagingFactory> Wic()
{
	EnsureCom();
	ComPtr<IWICImagingFactory> f;
	CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
	return f;
}

bool ImageSize(const std::wstring& path, int& w, int& h)
{
	ComPtr<IWICImagingFactory> f = Wic();
	if (!f) return false;
	ComPtr<IWICBitmapDecoder> dec;
	if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
	ComPtr<IWICBitmapFrameDecode> fr;
	UINT uw = 0, uh = 0;
	if (FAILED(dec->GetFrame(0, &fr)) || FAILED(fr->GetSize(&uw, &uh)) || !uw || !uh) return false;
	w = (int)uw;
	h = (int)uh;
	return true;
}

std::shared_ptr<Frame> DecodeImage(const std::wstring& path)
{
	ComPtr<IWICImagingFactory> f = Wic();
	if (!f) return nullptr;
	ComPtr<IWICBitmapDecoder> dec;
	if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return nullptr;
	ComPtr<IWICBitmapFrameDecode> fr;
	if (FAILED(dec->GetFrame(0, &fr))) return nullptr;
	ComPtr<IWICFormatConverter> conv;
	if (FAILED(f->CreateFormatConverter(&conv))) return nullptr;
	if (FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return nullptr;
	UINT w = 0, h = 0;
	if (FAILED(conv->GetSize(&w, &h)) || !w || !h || w > 16384 || h > 16384) return nullptr;
	auto out = std::make_shared<Frame>();
	out->w = (int)w;
	out->h = (int)h;
	out->bgra.resize((size_t)w * h * 4);
	if (FAILED(conv->CopyPixels(nullptr, w * 4, (UINT)out->bgra.size(), out->bgra.data()))) return nullptr;
	return out;
}

// ---- video cache folders
bool ReadCacheInfo(const std::wstring& dir, Info& info)
{
	FILE* f = nullptr;
	if (_wfopen_s(&f, (dir + L"\\info.txt").c_str(), L"rb") != 0 || !f) return false;
	int w = 0, h = 0, n = 0;
	double fps = 0;
	const int got = fscanf_s(f, "%d %d %d %lf", &w, &h, &n, &fps);
	fclose(f);
	if (got != 4 || w <= 0 || h <= 0 || n <= 0) return false;
	info.w = w;
	info.h = h;
	info.frames = n;
	info.fps = fps > 0 ? fps : 30.0;
	return true;
}

int CountFrames(const std::wstring& dir)
{
	int n = 0;
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"\\f*.png").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return 0;
	do { ++n; } while (FindNextFileW(h, &fd));
	FindClose(h);
	return n;
}

void RemoveTree(const std::wstring& dir)
{
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			const std::wstring name = fd.cFileName;
			if (name == L"." || name == L"..") continue;
			const std::wstring p = dir + L"\\" + name;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(p); else DeleteFileW(p.c_str());
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir.c_str());
}

std::string ReadWholeFile(const std::wstring& path)
{
	std::string s;
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return s;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
	fclose(f);
	return s;
}

// Runs a command line without a window; stderr goes to logPath. Returns the process handle (or null).
HANDLE Spawn(const std::wstring& cmd, const std::wstring& logPath)
{
	SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
	HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
	STARTUPINFOW si{ sizeof(si) };
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	si.hStdInput = nul;
	si.hStdOutput = nul;
	si.hStdError = log;
	PROCESS_INFORMATION pi{};
	std::wstring mutableCmd = cmd;
	const BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
	if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
	if (!ok) return nullptr;
	CloseHandle(pi.hThread);
	return pi.hProcess;
}

// Frame rate of the first video stream, from what `ffmpeg -i` prints.
double ProbeFps(const std::wstring& ffmpeg, const std::wstring& path, const std::wstring& scratch)
{
	const std::wstring log = scratch + L"\\probe.txt";
	HANDLE p = Spawn(L"\"" + ffmpeg + L"\" -hide_banner -i \"" + path + L"\"", log);
	if (!p) return 30.0;
	WaitForSingleObject(p, 20000);
	CloseHandle(p);
	const std::string text = ReadWholeFile(log);
	std::smatch m;
	const std::regex re("Video:[^\\n]*?(\\d+(?:\\.\\d+)?) fps");
	if (std::regex_search(text, m, re)) {
		const double v = atof(m[1].str().c_str());
		if (v > 0.5 && v < 1000) return v;
	}
	return 30.0;
}

std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

// Converts a video into <dir>\f00000.png ... and info.txt.
Info Convert(const std::wstring& path, const std::wstring& dir, const std::function<void(int)>& progress)
{
	Info info;
	info.video = true;
	const std::wstring ffmpeg = FindFfmpeg();
	if (ffmpeg.empty()) {
		info.error = L"ffmpeg.exe が見つかりません（C:\\FFmpeg\\bin など。環境変数 TDP_FFMPEG でも指定できます）";
		return info;
	}
	const std::wstring tmp = dir + L".tmp";
	RemoveTree(tmp);
	SHCreateDirectoryExW(nullptr, tmp.c_str(), nullptr);

	const double fps = ProbeFps(ffmpeg, path, tmp);
	// Alpha is kept (PNG rgba). Very large frames are reduced: textures do not need more than that.
	std::wstring cmd = Quote(ffmpeg) + L" -hide_banner -loglevel error -y -i " + Quote(path)
		+ L" -an -sn -map 0:v:0 -vf \"scale=w=min(iw\\,2560):h=-2:flags=bicubic,format=rgba\" -vsync 0 -compression_level 2 -start_number 0 "
		+ Quote(tmp + L"\\f%05d.png");
	HANDLE p = Spawn(cmd, tmp + L"\\ffmpeg.log");
	if (!p) {
		info.error = L"ffmpeg.exe を起動できませんでした";
		RemoveTree(tmp);
		return info;
	}
	while (WaitForSingleObject(p, 200) == WAIT_TIMEOUT) {
		if (progress) progress(CountFrames(tmp));
	}
	DWORD code = 1;
	GetExitCodeProcess(p, &code);
	CloseHandle(p);
	const int n = CountFrames(tmp);
	if (code != 0 || n <= 0) {
		std::string log = ReadWholeFile(tmp + L"\\ffmpeg.log");
		if (log.size() > 300) log.erase(0, log.size() - 300);
		std::wstring w(log.begin(), log.end());
		info.error = L"動画を変換できませんでした（ffmpeg の終了コード " + std::to_wstring(code) + L"）" + (w.empty() ? L"" : L"\n" + w);
		RemoveTree(tmp);
		return info;
	}
	int w = 0, h = 0;
	if (!ImageSize(tmp + L"\\f00000.png", w, h)) {
		info.error = L"変換した画像を読めませんでした";
		RemoveTree(tmp);
		return info;
	}
	if (FILE* f = nullptr; _wfopen_s(&f, (tmp + L"\\info.txt").c_str(), L"wb") == 0 && f) {
		fprintf(f, "%d %d %d %.6f\n", w, h, n, fps);
		fclose(f);
	}
	DeleteFileW((tmp + L"\\probe.txt").c_str());
	DeleteFileW((tmp + L"\\ffmpeg.log").c_str());
	RemoveTree(dir);
	if (!MoveFileExW(tmp.c_str(), dir.c_str(), 0)) {
		info.error = L"変換結果を保存できませんでした";
		RemoveTree(tmp);
		return info;
	}
	info.ok = true;
	info.w = w;
	info.h = h;
	info.frames = n;
	info.fps = fps;
	return info;
}

// Finds out what the file is. Fills `entry`; returns false (info.error set) when it cannot be used.
bool Examine(const std::wstring& path, Entry& entry)
{
	Info& info = entry.info;
	if (IsBuiltin(path)) { // generated, no file
		entry.key = path;
		info.id = path;
		info.ok = true;
		info.w = info.h = kNoiseSize;
		return true;
	}
	ULONGLONG size = 0, mtime = 0;
	if (path.empty() || !Stamp(path, size, mtime)) {
		info.error = path.empty() ? L"ファイルが選ばれていません" : L"ファイルが見つかりません";
		return false;
	}
	entry.key = KeyFor(path, size, mtime);
	info.id = entry.key;
	if (IsStill(path)) {
		int w = 0, h = 0;
		if (!ImageSize(path, w, h)) {
			info.error = L"画像を読めませんでした";
			return false;
		}
		info.ok = true;
		info.w = w;
		info.h = h;
		return true;
	}
	info.video = true;
	entry.dir = CacheRoot() + L"\\" + entry.key;
	if (ReadCacheInfo(entry.dir, info)) {
		info.ok = true;
		return true;
	}
	return false; // a video that has to be converted first (info.error empty)
}

} // namespace

std::wstring FindFfmpeg()
{
	auto exists = [](const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
	wchar_t buf[MAX_PATH * 2];
	DWORD n = GetEnvironmentVariableW(L"TDP_FFMPEG", buf, ARRAYSIZE(buf));
	if (n > 0 && n < ARRAYSIZE(buf) && exists(buf)) return buf;
	n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
	if (n > 0 && n < ARRAYSIZE(buf)) {
		std::wstring dir = buf;
		dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
		if (exists(dir + L"ffmpeg.exe")) return dir + L"ffmpeg.exe";
	}
	PWSTR pd = nullptr;
	std::wstring programData;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &pd))) programData = pd;
	CoTaskMemFree(pd);
	if (!programData.empty() && exists(programData + L"\\NAKA\\TitleDesignerPro\\ffmpeg.exe")) return programData + L"\\NAKA\\TitleDesignerPro\\ffmpeg.exe";
	wchar_t found[MAX_PATH * 2];
	if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, ARRAYSIZE(found), found, nullptr) > 0) return found;
	if (exists(L"C:\\FFmpeg\\bin\\ffmpeg.exe")) return L"C:\\FFmpeg\\bin\\ffmpeg.exe";
	return L"";
}

Info Lookup(const std::wstring& path)
{
	const std::wstring k = Lower(path);
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_entries.find(k);
		if (it != g_entries.end()) return it->second.info;
	}
	Entry e;
	const bool ok = Examine(path, e);
	std::lock_guard<std::mutex> lock(g_mutex);
	if (ok) g_entries[k] = e;
	e.info.converting = g_converting.count(k) > 0;
	return e.info;
}

Info Prepare(const std::wstring& path, const std::function<void(int)>& progress)
{
	const std::wstring k = Lower(path);
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_entries.erase(k); // look at the file again
	}
	Entry e;
	if (Examine(path, e) || !e.info.error.empty()) {
		std::lock_guard<std::mutex> lock(g_mutex);
		if (e.info.ok) g_entries[k] = e;
		return e.info;
	}
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_converting.insert(k);
		g_progress[k] = 0;
	}
	Info info = Convert(path, e.dir, [&](int n) {
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_progress[k] = n;
		}
		if (progress) progress(n);
	});
	info.id = e.key;
	std::lock_guard<std::mutex> lock(g_mutex);
	g_converting.erase(k);
	g_progress.erase(k);
	if (info.ok) {
		e.info = info;
		g_entries[k] = e;
	}
	return info;
}

void Request(const std::wstring& path)
{
	const std::wstring k = Lower(path);
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_requested.insert(k).second) return;
	}
	std::thread([path]() { Prepare(path); }).detach();
}

int ConvertedFrames(const std::wstring& path)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	auto it = g_progress.find(Lower(path));
	return it == g_progress.end() ? 0 : it->second;
}

std::shared_ptr<const Frame> GetFrame(const std::wstring& path, int index)
{
	const std::wstring k = Lower(path);
	std::wstring key, file;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_entries.find(k);
		if (it == g_entries.end()) return nullptr;
		const Entry& e = it->second;
		index = std::clamp(index, 0, std::max(e.info.frames - 1, 0));
		if (e.info.video) {
			wchar_t name[40];
			swprintf_s(name, L"\\f%05d.png", index);
			file = e.dir + name;
		} else {
			file = path;
		}
		key = e.key + L"#" + std::to_wstring(index);
		for (auto f = g_frames.begin(); f != g_frames.end(); ++f)
			if (f->key == key) {
				g_frames.splice(g_frames.begin(), g_frames, f);
				return g_frames.front().frame;
			}
	}
	std::shared_ptr<Frame> frame = IsBuiltin(path) ? MakeNoise(path) : DecodeImage(file);
	if (!frame) return nullptr;
	std::lock_guard<std::mutex> lock(g_mutex);
	g_frames.push_front({ key, frame });
	g_frameBytes += frame->bgra.size();
	while (g_frameBytes > kFrameBudget && g_frames.size() > 2) {
		g_frameBytes -= g_frames.back().frame->bgra.size();
		g_frames.pop_back();
	}
	return frame;
}

} // namespace media
} // namespace tdp
