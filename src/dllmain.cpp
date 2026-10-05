// Proxy de dinput8.dll: OLGame.exe importa DirectInput8Create, así que Windows
// carga esta DLL desde Binaries\Win64 antes que la del sistema. Reenviamos las
// exportaciones a la dinput8.dll real y arrancamos el mod.
#include <windows.h>
#include <unknwn.h>
#include "mod.h"

static HMODULE g_real = nullptr;

static FARPROC Real(const char* name) {
    if (!g_real) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\dinput8.dll");
        g_real = LoadLibraryW(path);
    }
    return g_real ? GetProcAddress(g_real, name) : nullptr;
}

extern "C" {
HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID iid, LPVOID* out, IUnknown* outer) {
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto f = (Fn)Real("DirectInput8Create");
    return f ? f(inst, ver, iid, out, outer) : E_FAIL;
}
HRESULT WINAPI Proxy_DllCanUnloadNow() {
    auto f = (HRESULT(WINAPI*)())Real("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}
HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID c, REFIID i, LPVOID* o) {
    auto f = (HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*))Real("DllGetClassObject");
    return f ? f(c, i, o) : E_FAIL;
}
HRESULT WINAPI Proxy_DllRegisterServer() {
    auto f = (HRESULT(WINAPI*)())Real("DllRegisterServer");
    return f ? f() : E_FAIL;
}
HRESULT WINAPI Proxy_DllUnregisterServer() {
    auto f = (HRESULT(WINAPI*)())Real("DllUnregisterServer");
    return f ? f() : E_FAIL;
}
}

BOOL WINAPI DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        mod::Startup(self);
    }
    return TRUE;
}
