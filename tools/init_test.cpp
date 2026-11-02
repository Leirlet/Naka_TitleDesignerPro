// Calls the deployed impl DLL's imInit the way the loader does.
#include <windows.h>
#include <cstdio>
#include <shlobj.h>
#include <string>
typedef int (*Entry)(int, void*, void*, void*);
int main()
{
	PWSTR base = nullptr;
	SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base);
	std::wstring p = std::wstring(base) + L"\\NAKA\\TitleDesignerPro\\NAKAfx_TitleDesignerPro_impl.dll";
	HMODULE m = LoadLibraryExW(p.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	printf("module %p err %lu\n", m, GetLastError());
	if (!m) return 1;
	Entry e = (Entry)GetProcAddress(m, "TdpImportEntry");
	printf("entry %p\n", e);
	static char stdParms[4096] = {}, info[4096] = {};
	int r = e(0 /* imInit */, stdParms, info, nullptr);
	printf("imInit -> %d\n", r);
	return 0;
}
