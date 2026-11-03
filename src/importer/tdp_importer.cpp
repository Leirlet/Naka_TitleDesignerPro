// NAKA Title Designer Pro - Premiere Pro custom importer (implementation DLL)
//
// Loaded by the thin loader .prm in MediaCore (see tdp_loader.cpp), which forwards
// every selector to TdpImportEntry. Based on the SDK_Custom_Import sample flow:
//   File > New > NAKA Title Designer Pro  -> imGetPrefs8 (create)  -> designer exe -> imSetFile
//   double-click the clip                -> imGetPrefs8 (modify)  -> designer exe -> RefreshFileAsync

#include <windows.h>
#include <shlobj.h>

#include "PrSDKImport.h"
#include "PrSDKImporterFileManagerSuite.h"
#include "PrSDKPixelFormat.h"
#include "PrSDKSequenceRenderSuite.h"
#include "PrSDKAlphaTypes.h"
#include "SPBasic.h"

#include "tdp_doc.h"
#include "tdp_render.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>

#define PIPL_NAME_UTF		L"NAKA Title Designer Pro"
#define FORMAT_NAME			"NAKA Title Designer Pro"
#define FORMAT_SHORT_NAME	"NAKA TDP"
#define TDP_FILETYPE		'NTDP'
#define DESIGNER_EXE		L"NAKA_TitleDesignerPro.exe"

namespace {

struct LocalRec {
	tdp::Doc*	doc;
	imFileRef	fileRef;
	wchar_t		path[MAX_PATH];
};
typedef LocalRec** LocalRecH;

// ---------------------------------------------------------------- logging
void Log(const wchar_t* fmt, ...)
{
	wchar_t path[MAX_PATH];
	DWORD n = GetTempPathW(MAX_PATH, path);
	if (n == 0 || n >= MAX_PATH) return;
	wcscat_s(path, L"NAKA_TDP_importer.log");

	wchar_t line[2048];
	SYSTEMTIME st;
	GetLocalTime(&st);
	int n0 = swprintf_s(line, L"%02d:%02d:%02d.%03d [%lu:%lu] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
		GetCurrentProcessId(), GetCurrentThreadId());
	va_list ap;
	va_start(ap, fmt);
	_vsnwprintf_s(line + n0, _countof(line) - n0 - 2, _TRUNCATE, fmt, ap);
	va_end(ap);
	wcscat_s(line, L"\r\n");

	char utf8[4096];
	int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr) - 1;
	if (bytes <= 0) return;
	// Share everything: viewers such as `tail -F` must not make logging fail.
	HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return;
	DWORD written;
	WriteFile(f, utf8, (DWORD)bytes, &written, nullptr);
	CloseHandle(f);
}

// ---------------------------------------------------------------- paths
std::wstring StripLongPrefix(const wchar_t* p)
{
	std::wstring s = p ? p : L"";
	if (s.rfind(L"\\\\?\\", 0) == 0) s.erase(0, 4);
	return s;
}

std::wstring ModuleDir()
{
	HMODULE mod = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&ModuleDir), &mod);
	wchar_t buf[MAX_PATH];
	GetModuleFileNameW(mod, buf, MAX_PATH);
	std::wstring s = buf;
	return s.substr(0, s.find_last_of(L'\\') + 1);
}

std::wstring DesignerPath()
{
	return ModuleDir() + DESIGNER_EXE;
}

std::wstring NewTitlePath()
{
	PWSTR docs = nullptr;
	std::wstring dir;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) dir = docs;
	CoTaskMemFree(docs);
	dir += L"\\NAKA Title Designer Pro\\Titles\\";
	SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

	SYSTEMTIME st;
	GetLocalTime(&st);
	wchar_t base[64];
	swprintf_s(base, L"Title_%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	std::wstring path = dir + base + L".tdp";
	for (int i = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++i)
		path = dir + base + L"_" + std::to_wstring(i) + L".tdp";
	return path;
}

// ---------------------------------------------------------------- background frame
// Writes a PPix as a 32bpp BMP (bottom-up, same row order as the PPix) and disposes it.
bool WriteBackgroundBmp(PlugppixFuncsPtr ppixFuncs, PPixHand ppix, const std::wstring& path)
{
	const char* pixels = ppixFuncs->ppixGetPixels(ppix);
	const int rowBytes = ppixFuncs->ppixGetRowbytes(ppix);
	prRect bounds;
	ppixFuncs->ppixGetBounds(ppix, &bounds);
	const int w = bounds.right - bounds.left;
	const int h = bounds.bottom - bounds.top;

	bool ok = false;
	HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f != INVALID_HANDLE_VALUE && pixels && w > 0 && h > 0) {
		BITMAPFILEHEADER fh{};
		BITMAPINFOHEADER ih{};
		ih.biSize = sizeof(ih);
		ih.biWidth = w;
		ih.biHeight = h;
		ih.biPlanes = 1;
		ih.biBitCount = 32;
		ih.biCompression = BI_RGB;
		fh.bfType = 0x4D42;
		fh.bfOffBits = sizeof(fh) + sizeof(ih);
		fh.bfSize = fh.bfOffBits + (DWORD)w * 4 * h;
		DWORD wr;
		WriteFile(f, &fh, sizeof(fh), &wr, nullptr);
		WriteFile(f, &ih, sizeof(ih), &wr, nullptr);
		for (int y = 0; y < h; ++y)
			WriteFile(f, pixels + (size_t)y * rowBytes, (DWORD)w * 4, &wr, nullptr);
		ok = true;
	}
	if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
	ppixFuncs->ppixDispose(ppix);
	Log(L"background: %s %dx%d rowBytes=%d", ok ? L"saved" : L"failed", w, h, rowBytes);
	return ok;
}

// Renders the playhead frame through the sequence render suite on a worker thread. Rendering the
// timeline also asks for the title clip that is being edited, which has hung Premiere when done on
// the calling thread, so the caller only waits a short time and gives up on timeout.
struct RenderJob {
	PrSDKSequenceRenderSuite* render = nullptr;
	PlugppixFuncsPtr ppixFuncs = nullptr;
	PrTimelineID timeline = 0;
	PrTime time = 0;
	int width = 0, height = 0, parNum = 1, parDen = 1;
	std::wstring path;
	SPBasicSuite* basic = nullptr;
	volatile LONG state = 0; // 0 running, 1 saved, 2 failed
	volatile LONG refs = 2;  // the caller and the worker thread; the last one out frees the job
};

// Drops one reference; the last owner releases the render suite and frees the job.
void ReleaseRenderJob(RenderJob* job)
{
	if (InterlockedDecrement(&job->refs) != 0) return;
	job->basic->ReleaseSuite(kPrSDKSequenceRenderSuite, kPrSDKSequenceRenderSuiteVersion);
	delete job;
}

DWORD WINAPI RenderJobThread(LPVOID arg)
{
	RenderJob* job = static_cast<RenderJob*>(arg);
	PrPixelFormat format = PrPixelFormat_BGRA_4444_8u;
	LONG result = 2;
	csSDK_uint32 rid = 0;
	prSuiteError re = job->render->MakeVideoRendererForTimeline(job->timeline, &rid);
	if (re == suiteError_NoError) {
		SequenceRender_ParamsRec params{};
		params.inRequestedPixelFormatArray = &format;
		params.inRequestedPixelFormatArrayCount = 1;
		params.inWidth = job->width;
		params.inHeight = job->height;
		params.inPixelAspectRatioNumerator = job->parNum;
		params.inPixelAspectRatioDenominator = job->parDen;
		params.inRenderQuality = kPrRenderQuality_Max;
		params.inFieldType = prFieldsNone;
		params.inDeinterlace = 0;
		params.inDeinterlaceQuality = kPrRenderQuality_Max;
		params.inCompositeOnBlack = 0;
		SequenceRender_GetFrameReturnRec ret{};
		re = job->render->RenderVideoFrame(rid, job->time, &params, kRenderCacheType_None, &ret);
		Log(L"background: RenderVideoFrame time=%lld err=%d ret=%d frame=%p", (long long)job->time, (int)re, (int)ret.returnVal, ret.outFrame);
		if (re == suiteError_NoError && ret.outFrame && WriteBackgroundBmp(job->ppixFuncs, ret.outFrame, job->path)) result = 1;
	} else {
		Log(L"background: MakeVideoRendererForTimeline err=%d", (int)re);
	}
	if (rid) job->render->ReleaseVideoRenderer(0, rid);
	InterlockedExchange(&job->state, result);
	ReleaseRenderJob(job);
	return 0;
}

// Saves the timeline frame under the clip as a 32bpp BMP for the designer's backdrop.
// With `pending` set (editing an existing clip), a render that has not finished after a short wait is
// left running: the file is written when Premiere lets it finish, and *pending tells the caller so.
std::wstring CaptureBackground(imStdParms* stdParms, imGetPrefsRec* prefsRec, bool* pending = nullptr)
{
	if (!prefsRec->timelineData || !stdParms->piSuites->timelineFuncs) {
		Log(L"background: no timelineData");
		return L"";
	}
	const TDB_TimeRecord& loc = prefsRec->tdbTimelineLocation;
	auto* ppixFuncs = stdParms->piSuites->ppixFuncs;

	wchar_t tmp[MAX_PATH];
	GetTempPathW(MAX_PATH, tmp);
	// A new name per capture, so nothing (file or WebView cache) can hand back an earlier frame.
	std::wstring dirTmp = tmp;
	WIN32_FIND_DATAW fd;
	if (HANDLE fh = FindFirstFileW((dirTmp + L"NAKA_TDP_background*.bmp").c_str(), &fd); fh != INVALID_HANDLE_VALUE) {
		do { DeleteFileW((dirTmp + fd.cFileName).c_str()); } while (FindNextFileW(fh, &fd));
		FindClose(fh);
	}
	const std::wstring path = dirTmp + L"NAKA_TDP_background_" + std::to_wstring(GetTickCount64()) + L".bmp";

	// getPreviewFrameEx ignores its frame argument (always the sequence's first frame), so try the
	// sequence render suite at the playhead time first. value / scale = seconds.
	SPBasicSuite* basic = stdParms->piSuites->utilFuncs->getSPBasicSuite();
	PrSDKSequenceRenderSuite* render = nullptr;
	if (basic && loc.scale > 0 && basic->AcquireSuite(kPrSDKSequenceRenderSuite, kPrSDKSequenceRenderSuiteVersion,
		(const void**)&render) == kSPNoError && render) {
		RenderJob* job = new RenderJob;
		job->render = render;
		job->ppixFuncs = ppixFuncs;
		job->timeline = prefsRec->timelineData;
		job->time = (PrTime)((double)loc.value / (double)loc.scale * 254016000000.0 + 0.5);
		job->width = prefsRec->imageWidth;
		job->height = prefsRec->imageHeight;
		job->parNum = prefsRec->pixelAspectNum;
		job->parDen = prefsRec->pixelAspectDen;
		job->path = path;
		job->basic = basic;

		HANDLE th = CreateThread(nullptr, 0, RenderJobThread, job, 0, nullptr);
		if (th) {
			const DWORD deadline = GetTickCount() + (pending ? 400 : 3000);
			while (job->state == 0) {
				const DWORD now = GetTickCount();
				if ((int)(deadline - now) <= 0) break;
				MsgWaitForMultipleObjects(1, &th, FALSE, deadline - now, QS_ALLINPUT);
				MSG msg;
				while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
					TranslateMessage(&msg);
					DispatchMessageW(&msg);
				}
				if (WaitForSingleObject(th, 0) == WAIT_OBJECT_0) break;
			}
			CloseHandle(th);
		}
		const LONG st = job->state;
		ReleaseRenderJob(job); // the worker thread keeps it alive while it is still running
		if (st == 1) return path;
		if (st == 0) {
			if (pending) {
				Log(L"background: render not finished yet, the designer waits for the file");
				*pending = true;
				return path;
			}
			Log(L"background: RenderVideoFrame timed out, using the fallback");
		}
	}

	// Fallback: the old call (first frame of the sequence only).
	prRect rect;
	prSetRect(&rect, 0, 0, prefsRec->imageWidth, prefsRec->imageHeight);
	PrPixelFormat format = PrPixelFormat_BGRA_4444_8u;
	PPixHand ppix = nullptr;
	csSDK_int32 frame = loc.sampleSize > 0 ? (csSDK_int32)(loc.value / loc.sampleSize) : (csSDK_int32)loc.value;
	csSDK_int32 err = stdParms->piSuites->timelineFuncs->getPreviewFrameEx(prefsRec->timelineData, frame, &ppix, &rect,
		&format, 1, prefsRec->pixelAspectNum, prefsRec->pixelAspectDen, true);
	Log(L"background: getPreviewFrameEx fallback frame=%d err=%d ppix=%p", frame, err, ppix);
	if (err != 0 || !ppix) return L"";
	return WriteBackgroundBmp(ppixFuncs, ppix, path) ? path : L"";
}

// ---------------------------------------------------------------- designer process
// Runs the designer and waits for it. Premiere's main window is disabled meanwhile,
// like a modal dialog. Returns the exit code (0 = saved, 1 = cancelled, -1 = failed).
HWND MainWindow(imStdParms* stdParms)
{
	return stdParms->piSuites->windFuncs ? (HWND)stdParms->piSuites->windFuncs->getMainWnd() : nullptr;
}

bool LaunchDesigner(HWND owner, const std::wstring& file, const std::wstring& background, bool isNew, PROCESS_INFORMATION& pi)
{
	const std::wstring exe = DesignerPath();
	std::wstring cmd = L"\"" + exe + L"\" --file \"" + file + L"\"";
	if (!background.empty()) cmd += L" --bg \"" + background + L"\"";
	if (isNew) cmd += L" --new";
	if (owner) cmd += L" --owner " + std::to_wstring((unsigned long long)(UINT_PTR)owner);

	STARTUPINFOW si{ sizeof(si) };
	Log(L"designer: %s", cmd.c_str());
	if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
		Log(L"designer: CreateProcess failed %lu", GetLastError());
		std::wstring msg = L"デザイナーを起動できませんでした。\n" + exe;
		MessageBoxW(owner, msg.c_str(), PIPL_NAME_UTF, MB_OK | MB_ICONERROR);
		return false;
	}
	AllowSetForegroundWindow(pi.dwProcessId);
	return true;
}

int RunDesigner(imStdParms* stdParms, const std::wstring& file, const std::wstring& background, bool isNew)
{
	HWND owner = MainWindow(stdParms);
	PROCESS_INFORMATION pi{};
	if (!LaunchDesigner(owner, file, background, isNew, pi)) return -1;

	if (owner) EnableWindow(owner, FALSE);
	for (;;) {
		DWORD r = MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, INFINITE, QS_ALLINPUT);
		if (r == WAIT_OBJECT_0) break;
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				PostQuitMessage((int)msg.wParam);
				break;
			}
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
	if (owner) {
		EnableWindow(owner, TRUE);
		SetForegroundWindow(owner);
	}

	DWORD code = (DWORD)-1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	Log(L"designer: exit code %lu", code);
	return (int)code;
}

// Tells Premiere that the clip's file changed so it redraws it.
void RefreshClip(SPBasicSuite* basic, const std::wstring& filepath)
{
	PrSDKImporterFileManagerSuite* fileSuite = nullptr;
	if (basic && basic->AcquireSuite(kPrSDKImporterFileManagerSuite, kPrSDKImporterFileManagerSuiteVersion,
		(const void**)&fileSuite) == kSPNoError && fileSuite) {
		prSuiteError e = fileSuite->RefreshFileAsync(reinterpret_cast<const prUTF16Char*>(filepath.c_str()));
		Log(L"modify: RefreshFileAsync -> %d", (int)e);
		basic->ReleaseSuite(kPrSDKImporterFileManagerSuite, kPrSDKImporterFileManagerSuiteVersion);
	}
}

// Editing an existing clip while Premiere will not finish the background render until imGetPrefs8
// returns: start the designer, return at once, and refresh the clip from a thread when it exits.
struct AsyncDesigner {
	PROCESS_INFORMATION pi{};
	SPBasicSuite* basic = nullptr;
	std::wstring filepath; // as Premiere gave it, for RefreshFileAsync
	std::wstring key;
};

std::mutex g_openMutex;
std::set<std::wstring> g_openFiles;

DWORD WINAPI AsyncDesignerThread(LPVOID arg)
{
	AsyncDesigner* a = static_cast<AsyncDesigner*>(arg);
	WaitForSingleObject(a->pi.hProcess, INFINITE);
	DWORD code = (DWORD)-1;
	GetExitCodeProcess(a->pi.hProcess, &code);
	CloseHandle(a->pi.hThread);
	CloseHandle(a->pi.hProcess);
	Log(L"designer: exit code %lu (async)", code);
	{
		std::lock_guard<std::mutex> lock(g_openMutex);
		g_openFiles.erase(a->key);
	}
	// Even when cancelled: frames rendered meanwhile were transparent, so ask for a redraw.
	RefreshClip(a->basic, a->filepath);
	delete a;
	return 0;
}

bool StartDesignerAsync(imStdParms* stdParms, const std::wstring& rawPath, const std::wstring& file, const std::wstring& background)
{
	AsyncDesigner* a = new AsyncDesigner;
	if (!LaunchDesigner(MainWindow(stdParms), file, background, false, a->pi)) {
		delete a;
		return false;
	}
	a->basic = stdParms->piSuites->utilFuncs->getSPBasicSuite();
	a->filepath = rawPath;
	a->key = file;
	{
		std::lock_guard<std::mutex> lock(g_openMutex);
		g_openFiles.insert(file);
	}
	HANDLE th = CreateThread(nullptr, 0, AsyncDesignerThread, a, 0, nullptr);
	if (th) CloseHandle(th);
	return true;
}

// ---------------------------------------------------------------- selectors
prMALError SDKInit(imStdParms*, imImportInfoRec* info)
{
	info->addToMenu		= imMenuNew;
	info->noFile		= kPrTrue;
	info->canCreate		= kPrTrue;
	info->hasSetup		= kPrTrue;
	info->setupOnDblClk	= kPrTrue;
	info->canResize		= kPrTrue;	// vector source: render at whatever size is requested
	Log(L"imInit: %s", tdp::RendererDescription().c_str());
	// Not cacheable: when Premiere started from its plugin cache, the File > New entry was missing.
	// Running imInit on every launch also picks up a replaced impl DLL.
	return malNoError;
}

prMALError SDKGetIndFormat(imStdParms*, csSDK_size_t index, imIndFormatRec* rec)
{
	if (index != 0) return imBadFormatIndex;
	static const char ext[] = "tdp\0";
	rec->filetype = TDP_FILETYPE;
	strcpy_s(rec->FormatName, sizeof(rec->FormatName), FORMAT_NAME);
	strcpy_s(rec->FormatShortName, sizeof(rec->FormatShortName), FORMAT_SHORT_NAME);
	memcpy(rec->PlatformExtension, ext, sizeof(ext));
	return malNoError;
}

prMALError SDKGetPrefs8(imStdParms* stdParms, imFileAccessRec8* access, imGetPrefsRec* prefsRec)
{
	// First call: tell Premiere how much prefs memory we want (we keep everything in the .tdp file).
	if (!prefsRec->prefs) {
		prefsRec->prefsLength = 1;
		return malNoError;
	}

	const bool creating = access->filepath && wcscmp(reinterpret_cast<const wchar_t*>(access->filepath), PIPL_NAME_UTF) == 0;
	Log(L"imGetPrefs8: creating=%d firstTime=%d timeline=%p size=%dx%d rate=%d/%d path=%s",
		creating, (int)prefsRec->firstTime, prefsRec->timelineData, prefsRec->imageWidth, prefsRec->imageHeight,
		prefsRec->vidScale, prefsRec->vidSampleSize,
		access->filepath ? reinterpret_cast<const wchar_t*>(access->filepath) : L"(null)");

	if (creating) {
		const std::wstring path = NewTitlePath();
		const tdp::Doc doc = tdp::MakeDefaultDoc(prefsRec->imageWidth, prefsRec->imageHeight,
			prefsRec->vidScale, prefsRec->vidSampleSize);
		if (!tdp::SaveDoc(path, doc)) {
			Log(L"create: cannot write %s", path.c_str());
			return imCancel;
		}

		const std::wstring bg = CaptureBackground(stdParms, prefsRec);
		if (RunDesigner(stdParms, path, bg, true) != 0) {
			DeleteFileW(path.c_str());
			return imCancel;
		}

		access->newfilename = stdParms->piSuites->memFuncs->newPtrClear((csSDK_uint32)((path.size() + 1) * sizeof(prUTF16Char)));
		wcscpy_s(reinterpret_cast<wchar_t*>(access->newfilename), path.size() + 1, path.c_str());
		// Custom importers MUST return imSetFile after file creation.
		return imSetFile;
	}

	if (!prefsRec->firstTime) {
		// Double-click on an existing clip.
		const std::wstring path = StripLongPrefix(reinterpret_cast<const wchar_t*>(access->filepath));
		{
			std::lock_guard<std::mutex> lock(g_openMutex);
			if (g_openFiles.count(path)) {
				Log(L"modify: designer already open for %s", path.c_str());
				return malNoError;
			}
		}
		bool pending = false;
		const std::wstring bg = CaptureBackground(stdParms, prefsRec, &pending);
		if (pending) {
			StartDesignerAsync(stdParms, reinterpret_cast<const wchar_t*>(access->filepath), path, bg);
			return malNoError;
		}
		if (RunDesigner(stdParms, path, bg, false) == 0) {
			SPBasicSuite* basic = stdParms->piSuites->utilFuncs->getSPBasicSuite();
			PrSDKImporterFileManagerSuite* fileSuite = nullptr;
			if (basic && basic->AcquireSuite(kPrSDKImporterFileManagerSuite, kPrSDKImporterFileManagerSuiteVersion,
				(const void**)&fileSuite) == kSPNoError && fileSuite) {
				prSuiteError e = fileSuite->RefreshFileAsync(access->filepath);
				Log(L"modify: RefreshFileAsync -> %d", (int)e);
				basic->ReleaseSuite(kPrSDKImporterFileManagerSuite, kPrSDKImporterFileManagerSuiteVersion);
			}
		}
	}
	return malNoError;
}

LocalRecH EnsureLocal(imStdParms* stdParms, void** privateData)
{
	if (*privateData) return reinterpret_cast<LocalRecH>(*privateData);
	LocalRecH h = reinterpret_cast<LocalRecH>(stdParms->piSuites->memFuncs->newHandleClear(sizeof(LocalRec)));
	(*h)->doc = new tdp::Doc();
	(*h)->fileRef = imInvalidHandleValue;
	*privateData = h;
	return h;
}

prMALError SDKOpenFile8(imStdParms* stdParms, imFileRef* fileRef, imFileOpenRec8* openRec)
{
	LocalRecH local = EnsureLocal(stdParms, &openRec->privatedata);
	// Keep a shared handle only so Premiere sees the file as open; the designer may rewrite it meanwhile.
	HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(openRec->fileinfo.filepath), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	(*local)->fileRef = h;
	wcsncpy_s((*local)->path, StripLongPrefix(reinterpret_cast<const wchar_t*>(openRec->fileinfo.filepath)).c_str(), _TRUNCATE);
	*fileRef = h;
	return h == INVALID_HANDLE_VALUE ? imBadFile : malNoError;
}

prMALError SDKQuietFile(imStdParms*, imFileRef* fileRef, void*)
{
	if (fileRef && *fileRef != imInvalidHandleValue) {
		CloseHandle(*fileRef);
		*fileRef = imInvalidHandleValue;
	}
	return malNoError;
}

prMALError SDKCloseFile(imStdParms* stdParms, imFileRef* fileRef, void* privateData)
{
	SDKQuietFile(stdParms, fileRef, privateData);
	if (privateData) {
		LocalRecH local = reinterpret_cast<LocalRecH>(privateData);
		delete (*local)->doc;
		stdParms->piSuites->memFuncs->disposeHandle(reinterpret_cast<PrMemoryHandle>(local));
	}
	return malNoError;
}

prMALError SDKGetInfo8(imStdParms* stdParms, imFileAccessRec8* access, imFileInfoRec8* info)
{
	LocalRecH local = EnsureLocal(stdParms, &info->privatedata);
	tdp::Doc& doc = *(*local)->doc;
	if (!tdp::LoadDoc(reinterpret_cast<const wchar_t*>(access->filepath), doc)) {
		Log(L"imGetInfo8: cannot read %s", reinterpret_cast<const wchar_t*>(access->filepath));
		return imBadFile;
	}

	std::wstring name = StripLongPrefix(reinterpret_cast<const wchar_t*>(access->filepath));
	name = name.substr(name.find_last_of(L'\\') + 1);
	wcsncpy_s(reinterpret_cast<wchar_t*>(info->streamName), 256, name.c_str(), _TRUNCATE);

	info->hasVideo					= kPrTrue;
	info->hasAudio					= kPrFalse;
	info->vidInfo.subType			= imUncompressed;
	info->vidInfo.imageWidth		= doc.width;
	info->vidInfo.imageHeight		= doc.height;
	info->vidInfo.depth				= 32;
	info->vidInfo.fieldType			= prFieldsNone;
	info->vidInfo.alphaType			= alphaStraight;
	info->vidInfo.pixelAspectNum	= 1;
	info->vidInfo.pixelAspectDen	= 1;
	info->vidInfo.isStill			= kPrFalse;
	info->vidInfo.noDuration		= imNoDurationFalse;
	info->vidScale					= doc.vidScale;
	info->vidSampleSize				= doc.vidSampleSize;
	info->vidDuration				= doc.durationFrames * doc.vidSampleSize;

	Log(L"imGetInfo8: %s %dx%d %d/%d frames=%d", name.c_str(), doc.width, doc.height, doc.vidScale, doc.vidSampleSize, doc.durationFrames);
	return malNoError;
}

prMALError SDKImportImage(imStdParms*, imFileRef, imImportImageRec* rec)
{
	LocalRecH local = reinterpret_cast<LocalRecH>(rec->privatedata);
	if (!local || !(*local)->doc) return imOtherErr;
	if (rec->pixformat != PrPixelFormat_BGRA_4444_8u) {
		Log(L"imImportImage: unexpected pixformat %08X", (unsigned)rec->pixformat);
		return imUnsupported;
	}
	{
		// While the designer has this file open the clip is drawn transparent, so the backdrop the
		// designer renders from the timeline does not contain the old title.
		std::lock_guard<std::mutex> lock(g_openMutex);
		if (!g_openFiles.empty() && g_openFiles.count((*local)->path)) {
			for (int y = 0; y < rec->dstHeight; ++y)
				memset(reinterpret_cast<uint8_t*>(rec->pix) + (size_t)y * rec->rowbytes, 0, (size_t)rec->dstWidth * 4);
			return malNoError;
		}
	}
	const int frame = rec->sampleSize > 0 ? rec->pos / rec->sampleSize : 0;
	const bool ok = tdp::RenderFrame(*(*local)->doc, frame, rec->dstWidth, rec->dstHeight,
		reinterpret_cast<uint8_t*>(rec->pix), rec->rowbytes, true);
	return ok ? malNoError : imOtherErr;
}

prMALError SDKGetIndPixelFormat(imStdParms*, csSDK_size_t index, imIndPixelFormatRec* rec)
{
	if (index != 0) return imBadFormatIndex;
	rec->outPixelFormat = PrPixelFormat_BGRA_4444_8u;
	return malNoError;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) Log(L"impl DLL loaded");
	if (reason == DLL_PROCESS_DETACH) Log(L"impl DLL unloaded");
	return TRUE;
}

extern "C" __declspec(dllexport) csSDK_int32 TdpImportEntry(csSDK_int32 selector, imStdParms* stdParms, void* param1, void* param2)
{
	// Startup diagnostics: the first call of each selector (File > New entry went missing on some launches).
	static bool seen[128] = {};
	if (selector >= 0 && selector < 128 && !seen[selector]) {
		seen[selector] = true;
		Log(L"selector %d (first call)", selector);
	}
	switch (selector) {
	case imInit:				return SDKInit(stdParms, reinterpret_cast<imImportInfoRec*>(param1));
	case imGetIndFormat:		return SDKGetIndFormat(stdParms, reinterpret_cast<csSDK_size_t>(param1), reinterpret_cast<imIndFormatRec*>(param2));
	case imGetPrefs8:			return SDKGetPrefs8(stdParms, reinterpret_cast<imFileAccessRec8*>(param1), reinterpret_cast<imGetPrefsRec*>(param2));
	case imGetInfo8:			return SDKGetInfo8(stdParms, reinterpret_cast<imFileAccessRec8*>(param1), reinterpret_cast<imFileInfoRec8*>(param2));
	case imOpenFile8:			return SDKOpenFile8(stdParms, reinterpret_cast<imFileRef*>(param1), reinterpret_cast<imFileOpenRec8*>(param2));
	case imQuietFile:			return SDKQuietFile(stdParms, reinterpret_cast<imFileRef*>(param1), param2);
	case imCloseFile:			return SDKCloseFile(stdParms, reinterpret_cast<imFileRef*>(param1), param2);
	case imImportImage:			return SDKImportImage(stdParms, reinterpret_cast<imFileRef>(param1), reinterpret_cast<imImportImageRec*>(param2));
	case imGetIndPixelFormat:	return SDKGetIndPixelFormat(stdParms, reinterpret_cast<csSDK_size_t>(param1), reinterpret_cast<imIndPixelFormatRec*>(param2));
	case imGetSupports8:		return malSupports8;
	case imGetSupports7:		return malSupports7;
	}
	return imUnsupported;
}
