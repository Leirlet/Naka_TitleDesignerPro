// NAKA Title Designer Pro - designer (WebView2 UI host)
//
// Launched by the importer:
//   NAKA_TitleDesignerPro.exe --file "<.tdp>" [--bg "<bmp>"] [--new] [--owner <hwnd>] [--devtools]
// Exit code: 0 = saved, 1 = cancelled, 2 = bad arguments / unreadable file / no WebView2.
//
// The UI (ui/index.html) talks to this host with chrome.webview.postMessage (JSON).
// Preview frames are rendered here (GPU) and handed over as WebView2 shared buffers (RGBA).

#include <windows.h>
#include <dwrite.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl.h>

#include "WebView2.h"

#include "tdp_doc.h"
#include "tdp_json.h"
#include "tdp_media.h"
#include "tdp_render.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using tdp::json::Value;

namespace {

struct App {
	std::wstring file;
	std::wstring bgPath;
	bool isNew = false;
	bool devtools = false;
	HWND owner = nullptr;

	tdp::Doc doc;
	HWND wnd = nullptr;
	ComPtr<ICoreWebView2Environment> env;
	ComPtr<ICoreWebView2Controller> controller;
	ComPtr<ICoreWebView2> web;
	int exitCode = 1;
	bool allowClose = false;
};
App g;

std::wstring ExeDir()
{
	wchar_t buf[MAX_PATH];
	GetModuleFileNameW(nullptr, buf, MAX_PATH);
	std::wstring s = buf;
	return s.substr(0, s.find_last_of(L'\\') + 1);
}

std::wstring KnownFolder(REFKNOWNFOLDERID id)
{
	PWSTR p = nullptr;
	std::wstring s;
	if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = p;
	CoTaskMemFree(p);
	return s;
}

std::wstring SettingsDir()
{
	std::wstring dir = KnownFolder(FOLDERID_RoamingAppData) + L"\\NAKA\\TitleDesignerPro";
	SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
	return dir;
}

std::wstring PresetsPath() { return SettingsDir() + L"\\presets.json"; }
std::wstring WindowStatePath() { return SettingsDir() + L"\\window.txt"; }

// Window size / position / maximized state of the last session: "left top right bottom maximized".
struct SavedWindow { RECT rc{}; bool maximized = false; bool valid = false; };

SavedWindow LoadWindowState()
{
	SavedWindow w;
	std::string text;
	long l, t, r, b;
	int m = 0;
	if (tdp::ReadFileUtf8(WindowStatePath(), text) && sscanf_s(text.c_str(), "%ld %ld %ld %ld %d", &l, &t, &r, &b, &m) == 5) {
		w.rc = { l, t, r, b };
		w.maximized = m != 0;
		// Only trust it if the window would still be on a connected monitor.
		w.valid = r - l > 200 && b - t > 200 && MonitorFromRect(&w.rc, MONITOR_DEFAULTTONULL) != nullptr;
	}
	return w;
}

void SaveWindowState(HWND hwnd)
{
	WINDOWPLACEMENT wp{ sizeof(wp) };
	if (!GetWindowPlacement(hwnd, &wp)) return;
	const RECT& r = wp.rcNormalPosition;
	char buf[128];
	sprintf_s(buf, "%ld %ld %ld %ld %d", r.left, r.top, r.right, r.bottom, wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
	tdp::WriteFileUtf8(WindowStatePath(), buf);
}

void PostJson(const Value& v)
{
	if (!g.web) return;
	g.web->PostWebMessageAsJson(tdp::json::FromUtf8(v.dump(false)).c_str());
}

void PostError(const std::wstring& message)
{
	Value v = Value::Object();
	v.set("type", "error");
	v.set("message", message);
	PostJson(v);
}

std::wstring LocalizedName(IDWriteLocalizedStrings* localized, const wchar_t* locale)
{
	UINT32 index = 0;
	BOOL exists = FALSE;
	localized->FindLocaleName(locale, &index, &exists);
	if (!exists) return L"";
	UINT32 len = 0;
	localized->GetStringLength(index, &len);
	std::wstring name(len + 1, L'\0');
	localized->GetString(index, name.data(), len + 1);
	name.resize(len);
	return name;
}

// Installed fonts as Windows sees them (C:\Windows\Fonts and fonts installed for this user):
// [{ name, en, weights: [..], ja: bool }], sorted by display name.
Value FontList()
{
	struct Entry { std::wstring name, en; std::set<int> weights; bool ja = false; };
	std::vector<Entry> entries;
	ComPtr<IDWriteFactory> dw;
	ComPtr<IDWriteFontCollection> coll;
	if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf())))
		&& SUCCEEDED(dw->GetSystemFontCollection(&coll))) {
		for (UINT32 i = 0; i < coll->GetFontFamilyCount(); ++i) {
			ComPtr<IDWriteFontFamily> family;
			ComPtr<IDWriteLocalizedStrings> localized;
			if (FAILED(coll->GetFontFamily(i, &family)) || FAILED(family->GetFamilyNames(&localized))) continue;
			Entry e;
			e.en = LocalizedName(localized.Get(), L"en-us");
			e.name = LocalizedName(localized.Get(), L"ja-jp");
			if (e.name.empty()) e.name = e.en;
			// ".Hiragino ..." and the like are hidden system UI fonts; "@" names are vertical variants.
			if (e.name.empty() || e.name[0] == L'.' || e.name[0] == L'@') continue;
			for (UINT32 k = 0; k < family->GetFontCount(); ++k) {
				ComPtr<IDWriteFont> font;
				if (FAILED(family->GetFont(k, &font)) || font->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE) continue;
				if (font->GetStyle() == DWRITE_FONT_STYLE_NORMAL) e.weights.insert((int)font->GetWeight());
				BOOL has = FALSE;
				if (!e.ja && SUCCEEDED(font->HasCharacter(0x3042, &has)) && has) e.ja = true; // "あ"
			}
			entries.push_back(std::move(e));
		}
	}
	std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
	Value arr = Value::Array();
	for (const auto& e : entries) {
		Value v = Value::Object();
		v.set("name", e.name);
		if (e.en != e.name) v.set("en", e.en);
		Value w = Value::Array();
		for (int x : e.weights) w.push(Value((double)x));
		v.set("weights", w);
		v.set("ja", e.ja);
		arr.push(v);
	}
	return arr;
}

// File dialog for a texture (image or video). Empty if cancelled.
std::wstring PickTextureFile()
{
	std::wstring result;
	ComPtr<IFileOpenDialog> dlg;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return result;
	const COMDLG_FILTERSPEC types[] = {
		{ L"画像・動画", L"*.png;*.jpg;*.jpeg;*.jfif;*.bmp;*.tif;*.tiff;*.gif;*.webp;*.mov;*.mp4;*.m4v;*.webm;*.avi;*.mkv;*.mxf;*.mpg" },
		{ L"すべてのファイル", L"*.*" },
	};
	dlg->SetFileTypes(ARRAYSIZE(types), types);
	dlg->SetTitle(L"テクスチャにする画像・動画を選ぶ");
	if (SUCCEEDED(dlg->Show(g.wnd))) {
		ComPtr<IShellItem> item;
		PWSTR p = nullptr;
		if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
			result = p;
			CoTaskMemFree(p);
		}
	}
	return result;
}

// What the renderer can do with a texture file right now (never starts a conversion).
Value TextureInfoJson(const std::wstring& path)
{
	const tdp::media::Info info = tdp::media::Lookup(path);
	Value v = Value::Object();
	v.set("type", "textureInfo");
	v.set("path", path);
	v.set("ok", info.ok);
	v.set("video", info.video);
	// A video that is not converted yet: the page asks for it (prepareTexture) or the renderer starts it.
	v.set("converting", info.converting);
	v.set("needsConversion", !info.ok && info.video && info.error.empty() && !info.converting);
	v.set("done", tdp::media::ConvertedFrames(path));
	v.set("width", info.w);
	v.set("height", info.h);
	v.set("frames", info.frames);
	v.set("fps", info.fps);
	v.set("error", info.error);
	return v;
}

void SendInit()
{
	Value v = Value::Object();
	v.set("type", "init");
	v.set("doc", tdp::DocToJson(g.doc));
	v.set("file", g.file);
	v.set("isNew", g.isNew);
	v.set("renderer", tdp::RendererDescription());
	if (!g.bgPath.empty()) {
		std::wstring name = g.bgPath.substr(g.bgPath.find_last_of(L'\\') + 1);
		v.set("bgUrl", L"https://tdp.bg/" + name);
	}
	v.set("fonts", FontList());

	std::string presets;
	Value pv;
	if (tdp::ReadFileUtf8(PresetsPath(), presets) && tdp::json::Parse(presets, pv)) v.set("presets", pv);
	PostJson(v);
}

// Renders the doc in the message and posts the pixels (RGBA, straight alpha) as a shared buffer.
void HandleRender(const Value& m)
{
	tdp::Doc doc;
	if (!tdp::DocFromJson(m["doc"], doc)) return;
	const int w = std::clamp(m["width"].asInt(0), 1, 8192);
	const int h = std::clamp(m["height"].asInt(0), 1, 8192);
	const int frame = m["frame"].asInt(0);

	ComPtr<ICoreWebView2Environment12> env12;
	ComPtr<ICoreWebView2_17> web17;
	if (FAILED(g.env.As(&env12)) || FAILED(g.web.As(&web17))) return;
	ComPtr<ICoreWebView2SharedBuffer> buffer;
	if (FAILED(env12->CreateSharedBuffer((UINT64)w * h * 4, &buffer))) return;
	BYTE* pixels = nullptr;
	buffer->get_Buffer(&pixels);

	const auto t0 = std::chrono::steady_clock::now();
	const bool ok = tdp::RenderFrame(doc, frame, w, h, pixels, w * 4, false);
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	if (ok) {
		for (size_t i = 0, n = (size_t)w * h; i < n; ++i) std::swap(pixels[i * 4], pixels[i * 4 + 2]); // BGRA -> RGBA
	} else {
		memset(pixels, 0, (size_t)w * h * 4);
	}

	Value info = Value::Object();
	info.set("type", "frame");
	info.set("seq", m["seq"].asInt(0));
	info.set("purpose", m["purpose"].isString() ? m["purpose"].asString() : std::string("preview"));
	info.set("key", m["key"].isString() ? m["key"].asString() : std::string());
	info.set("width", w);
	info.set("height", h);
	info.set("ms", ms);
	info.set("ok", ok);
	if (m["purpose"].asString() != "thumb") {
		Value quads = Value::Array();
		for (size_t i = 0; i < doc.objects.size(); ++i) {
			float q[8] = {};
			Value qv = Value::Array();
			if (tdp::GetObjectQuad(doc, i, q))
				for (float f : q) qv.push(Value(f));
			quads.push(qv);
		}
		info.set("quads", quads);
		// Character cells of one object (for the kerning handles): the page asks with "cells": object index.
		if (m["cells"].isNumber() && m["cells"].asInt(-1) >= 0) {
			std::vector<tdp::CharCell> cells;
			Value cv = Value::Array();
			if (tdp::GetObjectCells(doc, (size_t)m["cells"].asInt(0), cells)) {
				for (const tdp::CharCell& c : cells) {
					Value one = Value::Object();
					one.set("ch", c.ch);
					Value q = Value::Array();
					for (float f : c.quad) q.push(Value(f));
					one.set("q", q);
					cv.push(one);
				}
			}
			info.set("cells", cv);
			info.set("cellsObject", m["cells"].asInt(0));
		}
	}
	web17->PostSharedBufferToScript(buffer.Get(), COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY,
		tdp::json::FromUtf8(info.dump(false)).c_str());
	// Our reference goes away here; the page releases its view with chrome.webview.releaseBuffer().
}

void CloseWith(int code)
{
	g.exitCode = code;
	g.allowClose = true;
	DestroyWindow(g.wnd);
}

void HandleMessage(const Value& m)
{
	const std::string& type = m["type"].asString();
	if (type == "ready") {
		SendInit();
	} else if (type == "render") {
		HandleRender(m);
	} else if (type == "save") {
		tdp::Doc doc;
		if (!tdp::DocFromJson(m["doc"], doc)) { PostError(L"保存するデータが壊れています。"); return; }
		if (!tdp::SaveDoc(g.file, doc)) { PostError(L"保存できませんでした。\n" + g.file); return; }
		CloseWith(0);
	} else if (type == "cancel") {
		CloseWith(1);
	} else if (type == "savePresets") {
		if (!tdp::WriteFileUtf8(PresetsPath(), m["data"].dump(true))) PostError(L"プリセットを保存できませんでした。");
	} else if (type == "devtools") {
		if (g.devtools) g.web->OpenDevToolsWindow();
	} else if (type == "pickTexture") {
		Value v = Value::Object();
		v.set("type", "texturePicked");
		v.set("id", m["id"].isString() ? m["id"].asString() : std::string());
		v.set("path", PickTextureFile());
		PostJson(v);
	} else if (type == "textureInfo") {
		PostJson(TextureInfoJson(m["path"].asWString(L"")));
	} else if (type == "prepareTexture") {
		// Makes the frame cache of a video in the background (stills need nothing); the page polls textureInfo.
		const std::wstring path = m["path"].asWString(L"");
		const tdp::media::Info info = tdp::media::Lookup(path);
		if (!info.ok && info.video && info.error.empty() && !info.converting)
			std::thread([path]() { tdp::media::Prepare(path); }).detach();
		PostJson(TextureInfoJson(path));
	}
}

void CreateWebView()
{
	const std::wstring userData = KnownFolder(FOLDERID_LocalAppData) + L"\\NAKA\\TitleDesignerPro\\WebView2";
	HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, userData.c_str(), nullptr,
		Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
			if (FAILED(result) || !env) {
				MessageBoxW(g.wnd, L"WebView2 ランタイムを起動できませんでした。", L"NAKA Title Designer Pro", MB_OK | MB_ICONERROR);
				CloseWith(2);
				return S_OK;
			}
			g.env = env;
			return env->CreateCoreWebView2Controller(g.wnd,
				Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
					if (FAILED(result) || !controller) {
						MessageBoxW(g.wnd, L"WebView2 を作成できませんでした。", L"NAKA Title Designer Pro", MB_OK | MB_ICONERROR);
						CloseWith(2);
						return S_OK;
					}
					g.controller = controller;
					controller->get_CoreWebView2(&g.web);

					RECT rc;
					GetClientRect(g.wnd, &rc);
					controller->put_Bounds(rc);

					ComPtr<ICoreWebView2Settings> settings;
					g.web->get_Settings(&settings);
					settings->put_AreDevToolsEnabled(g.devtools);
					settings->put_AreDefaultContextMenusEnabled(g.devtools);
					settings->put_IsStatusBarEnabled(FALSE);
					settings->put_IsZoomControlEnabled(FALSE);

					ComPtr<ICoreWebView2_3> web3;
					if (SUCCEEDED(g.web.As(&web3))) {
						web3->SetVirtualHostNameToFolderMapping(L"tdp.ui", (ExeDir() + L"ui").c_str(),
							COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
						if (!g.bgPath.empty()) {
							std::wstring dir = g.bgPath.substr(0, g.bgPath.find_last_of(L'\\'));
							web3->SetVirtualHostNameToFolderMapping(L"tdp.bg", dir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
						}
					}

					EventRegistrationToken token;
					g.web->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
						[](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
							LPWSTR raw = nullptr;
							if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw) {
								Value m;
								if (tdp::json::Parse(tdp::json::ToUtf8(raw), m)) HandleMessage(m);
								CoTaskMemFree(raw);
							}
							return S_OK;
						}).Get(), &token);

					g.web->Navigate(L"https://tdp.ui/index.html");
					return S_OK;
				}).Get());
		}).Get());
	if (FAILED(hr)) {
		MessageBoxW(g.wnd, L"WebView2 ランタイムが見つかりません。", L"NAKA Title Designer Pro", MB_OK | MB_ICONERROR);
		CloseWith(2);
	}
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_CREATE:
		g.wnd = hwnd;
		CreateWebView();
		return 0;
	case WM_SIZE:
		if (g.controller) {
			RECT rc;
			GetClientRect(hwnd, &rc);
			g.controller->put_Bounds(rc);
		}
		return 0;
	case WM_GETMINMAXINFO: {
		const float s = GetDpiForWindow(hwnd) / 96.0f;
		// Small enough for a narrow window next to vertical video; the panels shrink with it.
		reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = { (LONG)(640 * s), (LONG)(480 * s) };
		return 0;
	}
	case WM_CLOSE:
		// Let the page ask about unsaved changes; it answers with "cancel".
		if (!g.allowClose && g.web) {
			Value v = Value::Object();
			v.set("type", "requestClose");
			PostJson(v);
			return 0;
		}
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		SaveWindowState(hwnd);
		if (g.controller) g.controller->Close();
		PostQuitMessage(g.exitCode);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

bool ParseArgs()
{
	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	for (int i = 1; i < argc; ++i) {
		std::wstring a = argv[i];
		if (a == L"--file" && i + 1 < argc) g.file = argv[++i];
		else if (a == L"--bg" && i + 1 < argc) g.bgPath = argv[++i];
		else if (a == L"--new") g.isNew = true;
		else if (a == L"--devtools") g.devtools = true;
		else if (a == L"--owner" && i + 1 < argc) g.owner = (HWND)(UINT_PTR)_wcstoui64(argv[++i], nullptr, 10);
	}
	LocalFree(argv);
	return !g.file.empty();
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	if (!ParseArgs()) {
		MessageBoxW(nullptr, L"使い方: NAKA_TitleDesignerPro.exe --file <title.tdp> [--bg <frame.bmp>]",
			L"NAKA Title Designer Pro", MB_OK | MB_ICONINFORMATION);
		return 2;
	}
	if (!tdp::LoadDoc(g.file, g.doc)) {
		MessageBoxW(nullptr, (L"ファイルを読めませんでした。\n" + g.file).c_str(), L"NAKA Title Designer Pro", MB_OK | MB_ICONERROR);
		return 2;
	}

	WNDCLASSEXW wc{ sizeof(wc) };
	wc.lpfnWndProc = MainProc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = CreateSolidBrush(RGB(38, 38, 38));
	wc.lpszClassName = L"TdpDesigner";
	wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
	RegisterClassExW(&wc);

	// Center on the monitor that holds Premiere's main window (or the primary one).
	MONITORINFO mi{ sizeof(mi) };
	HMONITOR mon = MonitorFromWindow(g.owner, MONITOR_DEFAULTTOPRIMARY);
	GetMonitorInfoW(mon, &mi);
	UINT dpiX = 96, dpiY = 96;
	using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
	if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
		if (auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"))) fn(mon, 0, &dpiX, &dpiY);
	}
	const float s = dpiX / 96.0f;
	const int workW = mi.rcWork.right - mi.rcWork.left, workH = mi.rcWork.bottom - mi.rcWork.top;
	const int ww = std::min((int)(1600 * s), workW), wh = std::min((int)(960 * s), workH);
	const int x = mi.rcWork.left + (workW - ww) / 2;
	const int y = mi.rcWork.top + (workH - wh) / 2;

	std::wstring name = g.file.substr(g.file.find_last_of(L'\\') + 1);
	std::wstring title = L"NAKA Title Designer Pro - " + name;
	HWND wnd = CreateWindowExW(0, L"TdpDesigner", title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
		x, y, ww, wh, nullptr, nullptr, inst, nullptr);
	const SavedWindow saved = LoadWindowState();
	if (saved.valid) {
		WINDOWPLACEMENT wp{ sizeof(wp) };
		wp.showCmd = saved.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
		wp.rcNormalPosition = saved.rc;
		SetWindowPlacement(wnd, &wp);
	} else {
		ShowWindow(wnd, SW_SHOW);
	}
	SetForegroundWindow(wnd);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	CoUninitialize();
	return (int)msg.wParam;
}
