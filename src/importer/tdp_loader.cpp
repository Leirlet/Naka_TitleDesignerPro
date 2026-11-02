// NAKA Title Designer Pro - thin loader placed in MediaCore.
//
// Copying into MediaCore needs administrator rights, so this .prm stays tiny and stable.
// It forwards every selector to the implementation DLL in %ProgramData%\NAKA\TitleDesignerPro,
// which can be rebuilt and replaced without admin rights (Premiere restart needed).
// Not AppData: writes there from a packaged app (e.g. the Claude desktop app) are redirected
// into the package and stay invisible to a Premiere started from the Start menu.

#include <windows.h>
#include <shlobj.h>

#include "PrSDKImport.h"

#include <cstdio>
#include <string>

#define IMPL_DLL L"NAKAfx_TitleDesignerPro_impl.dll"

typedef csSDK_int32 (*TdpImportEntryFunc)(csSDK_int32, imStdParms*, void*, void*);

namespace {

std::wstring ImplDir()
{
	PWSTR base = nullptr;
	std::wstring dir;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &base))) dir = base;
	CoTaskMemFree(base);
	return dir + L"\\NAKA\\TitleDesignerPro\\";
}

// Same log file as the implementation DLL; written only when loading goes wrong.
void LoaderLog(const wchar_t* what, DWORD err)
{
	wchar_t path[MAX_PATH];
	DWORD n = GetTempPathW(MAX_PATH, path);
	if (n == 0 || n >= MAX_PATH) return;
	wcscat_s(path, L"NAKA_TDP_importer.log");
	SYSTEMTIME st;
	GetLocalTime(&st);
	char line[512];
	int len = sprintf_s(line, "%02d:%02d:%02d.%03d [%lu:%lu] loader: %ls (error %lu)\r\n", st.wHour, st.wMinute, st.wSecond,
		st.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId(), what, err);
	HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return;
	DWORD written;
	if (len > 0) WriteFile(f, line, (DWORD)len, &written, nullptr);
	CloseHandle(f);
}

// After a failed load: is the file visible to this process, and which import cannot be loaded?
void DiagnoseFailure(const std::wstring& path)
{
	const DWORD attrs = GetFileAttributesW(path.c_str());
	LoaderLog(attrs == INVALID_FILE_ATTRIBUTES ? L"diagnose: implementation DLL not visible" : L"diagnose: implementation DLL exists",
		attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0);
	// Imports of the implementation DLL (dumpbin /dependents).
	static const wchar_t* const kImports[] = { L"user32.dll", L"shell32.dll", L"ole32.dll", L"d3d11.dll", L"d2d1.dll", L"dwrite.dll" };
	for (const wchar_t* name : kImports) {
		HMODULE m = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (!m) {
			std::wstring what = L"diagnose: cannot load ";
			LoaderLog((what + name).c_str(), GetLastError());
		} else {
			FreeLibrary(m);
		}
	}
}

HMODULE TryLoad(const std::wstring& path, int attempt)
{
	HMODULE mod = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (mod) return mod;
	LoaderLog(path.c_str(), GetLastError());
	if (attempt == 0) DiagnoseFailure(path);
	// Fall back to the classic search order in case the restricted one is what fails.
	mod = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (mod) LoaderLog(L"loaded with LOAD_WITH_ALTERED_SEARCH_PATH", (DWORD)attempt);
	else if (attempt == 0) LoaderLog(L"LOAD_WITH_ALTERED_SEARCH_PATH failed too", GetLastError());
	return mod;
}

TdpImportEntryFunc LoadImpl()
{
	const std::wstring path = ImplDir() + IMPL_DLL;
	// The implementation DLL is sometimes not loadable right at startup; retry for a few seconds.
	for (int attempt = 0; attempt < 10; ++attempt) {
		HMODULE mod = TryLoad(path, attempt);
		if (mod) {
			auto entry = reinterpret_cast<TdpImportEntryFunc>(GetProcAddress(mod, "TdpImportEntry"));
			if (!entry) LoaderLog(L"TdpImportEntry not found", GetLastError());
			else if (attempt > 0) LoaderLog(L"loaded after retrying", (DWORD)attempt);
			return entry;
		}
		Sleep(300);
	}
	return nullptr;
}

} // namespace

extern "C" PREMPLUGENTRY DllExport xImportEntry(csSDK_int32 selector, imStdParms* stdParms, void* param1, void* param2)
{
	static TdpImportEntryFunc impl = LoadImpl();
	if (!impl) {
		if (selector == imInit) LoaderLog(L"imInit failed: implementation DLL not loaded", 0);
		return selector == imInit ? imOtherErr : imUnsupported;
	}
	return impl(selector, stdParms, param1, param2);
}
