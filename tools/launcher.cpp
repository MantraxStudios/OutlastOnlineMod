// Outlast Online Launcher
// Busca la instalación de Outlast, guarda la ruta, instala/actualiza el mod
// (dinput8.dll va embebida como recurso) y lanza el juego.
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Recurso 1 = DLL de 64 bits (Binaries\Win64), recurso 2 = DLL de 32 bits (Binaries\Win32)
struct Arch { const wchar_t* dir; int resource; };
static const Arch kArch[2] = {{L"\\Binaries\\Win64", 1}, {L"\\Binaries\\Win32", 2}};

enum { IdPlay = 101, IdBrowse, IdUninstall, IdArch, IdTimer };

static HWND g_wnd, g_status, g_path, g_btnPlay, g_btnBrowse, g_btnUninstall, g_btnArch;
static std::wstring g_gameDir;
static int g_arch = 0;  // 0 = 64 bits, 1 = 32 bits
static int g_countdown = -1;

// ---------------------------------------------------------------------------
static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::wstring ExePath(const std::wstring& d, int a) { return d + kArch[a].dir + L"\\OLGame.exe"; }
static std::wstring DllPath(const std::wstring& d, int a) { return d + kArch[a].dir + L"\\dinput8.dll"; }
static bool IsGameDir(const std::wstring& d) {
    return !d.empty() && (FileExists(ExePath(d, 0)) || FileExists(ExePath(d, 1)));
}

static std::wstring ConfigPath() {
    wchar_t buf[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf);
    std::wstring dir = std::wstring(buf) + L"\\OutlastOnline";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\launcher.ini";
}
static std::wstring LoadSavedPath() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetPrivateProfileStringW(L"Launcher", L"GamePath", L"", buf, MAX_PATH * 2, ConfigPath().c_str());
    return buf;
}
static void SavePath(const std::wstring& p) {
    WritePrivateProfileStringW(L"Launcher", L"GamePath", p.c_str(), ConfigPath().c_str());
}
static void SaveArch() {
    WritePrivateProfileStringW(L"Launcher", L"Arch", g_arch ? L"32" : L"64", ConfigPath().c_str());
}
static bool Is64BitWindows() {
    BOOL wow = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow);
    return sizeof(void*) == 8 || wow;
}
// Versión a lanzar: la guardada; si no, 64 bits cuando existe y Windows es de 64 bits
static void PickArch() {
    int saved = GetPrivateProfileIntW(L"Launcher", L"Arch", 0, ConfigPath().c_str());
    g_arch = saved == 32 ? 1 : 0;
    if (!saved && !Is64BitWindows()) g_arch = 1;
    if (!FileExists(ExePath(g_gameDir, g_arch)) && FileExists(ExePath(g_gameDir, 1 - g_arch))) g_arch = 1 - g_arch;
}

static std::wstring RegStr(HKEY root, const wchar_t* key, const wchar_t* val, REGSAM extra = 0) {
    wchar_t buf[MAX_PATH * 2];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(root, key, val, RRF_RT_REG_SZ | (extra ? 0 : 0), nullptr, buf, &sz) == ERROR_SUCCESS) return buf;
    HKEY h;
    if (RegOpenKeyExW(root, key, 0, KEY_READ | KEY_WOW64_32KEY, &h) == ERROR_SUCCESS) {
        sz = sizeof(buf);
        LONG r = RegQueryValueExW(h, val, nullptr, nullptr, (BYTE*)buf, &sz);
        RegCloseKey(h);
        if (r == ERROR_SUCCESS) return buf;
    }
    return L"";
}

static std::wstring Normalize(std::wstring p) {
    for (auto& c : p) if (c == L'/') c = L'\\';
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'"')) p.pop_back();
    return p;
}

// Rutas de las bibliotecas de Steam (libraryfolders.vdf)
static std::vector<std::wstring> SteamLibraries() {
    std::vector<std::wstring> libs;
    std::wstring steam = RegStr(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty()) steam = RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steam.empty()) steam = RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
    if (steam.empty()) return libs;
    steam = Normalize(steam);
    libs.push_back(steam);
    std::ifstream f(steam + L"\\steamapps\\libraryfolders.vdf");
    std::string line;
    while (std::getline(f, line)) {
        auto k = line.find("\"path\"");
        if (k == std::string::npos) continue;
        auto a = line.find('"', k + 6);
        auto b = line.rfind('"');
        if (a == std::string::npos || b <= a) continue;
        std::string v = line.substr(a + 1, b - a - 1);
        std::string un;  // "\\" -> "\"
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == '\\') ++i;
            un.push_back(v[i]);
        }
        int n = MultiByteToWideChar(CP_UTF8, 0, un.c_str(), -1, nullptr, 0);
        std::wstring w(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, un.c_str(), -1, &w[0], n);
        w.resize(n - 1);
        libs.push_back(Normalize(w));
    }
    return libs;
}

// Busca en las claves de desinstalación (GOG, Epic, instaladores)
static std::wstring FromUninstallKeys() {
    const wchar_t* roots[] = {L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                              L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"};
    for (HKEY hive : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        for (auto root : roots) {
            HKEY h;
            if (RegOpenKeyExW(hive, root, 0, KEY_READ, &h) != ERROR_SUCCESS) continue;
            wchar_t sub[256];
            for (DWORD i = 0;; ++i) {
                DWORD len = 256;
                if (RegEnumKeyExW(h, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                std::wstring key = std::wstring(root) + L"\\" + sub;
                std::wstring name = RegStr(hive, key.c_str(), L"DisplayName");
                if (name != L"Outlast" && name.rfind(L"Outlast ", 0) != 0) continue;
                std::wstring loc = Normalize(RegStr(hive, key.c_str(), L"InstallLocation"));
                if (IsGameDir(loc)) { RegCloseKey(h); return loc; }
            }
            RegCloseKey(h);
        }
    }
    return L"";
}

static std::wstring FindGame() {
    // 1) Ruta guardada
    std::wstring p = LoadSavedPath();
    if (IsGameDir(p)) return p;
    // 2) Si el launcher está dentro de la carpeta del juego
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring d = self;
    for (int i = 0; i < 4; ++i) {
        d = d.substr(0, d.find_last_of(L'\\'));
        if (IsGameDir(d)) return d;
    }
    // 3) Bibliotecas de Steam
    for (auto& lib : SteamLibraries()) {
        p = lib + L"\\steamapps\\common\\Outlast";
        if (IsGameDir(p)) return p;
    }
    // 4) Registro (GOG / Epic / otros)
    p = FromUninstallKeys();
    if (!p.empty()) return p;
    p = Normalize(RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\GOG.com\\Games\\1207659123", L"path"));
    if (IsGameDir(p)) return p;
    // 5) Rutas típicas en todas las unidades
    const wchar_t* rels[] = {L"\\SteamLibrary\\steamapps\\common\\Outlast",
                             L"\\Steam\\steamapps\\common\\Outlast",
                             L"\\Program Files (x86)\\Steam\\steamapps\\common\\Outlast",
                             L"\\Program Files\\Steam\\steamapps\\common\\Outlast",
                             L"\\Games\\Steam\\steamapps\\common\\Outlast",
                             L"\\Juegos\\Steam\\steamapps\\common\\Outlast",
                             L"\\Program Files (x86)\\GOG Galaxy\\Games\\Outlast",
                             L"\\GOG Games\\Outlast",
                             L"\\Program Files\\Epic Games\\Outlast",
                             L"\\Epic Games\\Outlast",
                             L"\\Games\\Outlast",
                             L"\\Juegos\\Outlast",
                             L"\\Outlast"};
    DWORD drives = GetLogicalDrives();
    for (int i = 2; i < 26; ++i) {
        if (!(drives & (1 << i))) continue;
        wchar_t root[] = {wchar_t(L'A' + i), L':', 0};
        if (GetDriveTypeW((std::wstring(root) + L"\\").c_str()) != DRIVE_FIXED) continue;
        for (auto r : rels) {
            p = std::wstring(root) + r;
            if (IsGameDir(p)) return p;
        }
    }
    return L"";
}

// ---------------------------------------------------------------------------
static bool ExtractDll(int resource, const std::wstring& dest, std::wstring& err) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(resource), RT_RCDATA);
    if (!r) { err = L"recurso no encontrado"; return false; }
    HGLOBAL g = LoadResource(nullptr, r);
    const void* data = LockResource(g);
    DWORD size = SizeofResource(nullptr, r);
    // ¿Ya está instalada la misma versión?
    std::ifstream in(dest, std::ios::binary);
    if (in) {
        std::vector<char> cur((std::istreambuf_iterator<char>(in)), {});
        if (cur.size() == size && !memcmp(cur.data(), data, size)) return true;
    }
    in.close();
    HANDLE h = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        err = e == ERROR_ACCESS_DENIED ? L"acceso denegado (ejecuta el launcher como administrador)"
            : e == ERROR_SHARING_VIOLATION ? L"el juego esta abierto, cierralo primero"
                                           : L"error " + std::to_wstring(e);
        return false;
    }
    DWORD w = 0;
    WriteFile(h, data, size, &w, nullptr);
    CloseHandle(h);
    return w == size;
}

static void SetStatus(const std::wstring& s) { SetWindowTextW(g_status, s.c_str()); }

static void UpdatePathLabel() {
    SetWindowTextW(g_path, g_gameDir.empty() ? L"Carpeta del juego: (no encontrada)"
                                             : (L"Carpeta del juego: " + g_gameDir).c_str());
    EnableWindow(g_btnPlay, !g_gameDir.empty());
    EnableWindow(g_btnUninstall, !g_gameDir.empty() && (FileExists(DllPath(g_gameDir, 0)) || FileExists(DllPath(g_gameDir, 1))));
    SetWindowTextW(g_btnArch, g_arch ? L"Version: 32 bits" : L"Version: 64 bits");
    EnableWindow(g_btnArch, !g_gameDir.empty() && FileExists(ExePath(g_gameDir, 0)) && FileExists(ExePath(g_gameDir, 1)) &&
                                Is64BitWindows());
}

static bool IsGameRunning() {
    HWND w = FindWindowW(nullptr, L"Outlast");
    return w != nullptr;
}

static void Play() {
    g_countdown = -1;
    KillTimer(g_wnd, IdTimer);
    if (!IsGameDir(g_gameDir)) { SetStatus(L"No se encontro OLGame.exe en esa carpeta."); return; }
    std::wstring err;
    SetStatus(L"Instalando Outlast Online...");
    // Instalar en las dos versiones que existan (asi funciona tambien si abres el juego desde Steam)
    for (int a = 0; a < 2; ++a) {
        if (!FileExists(ExePath(g_gameDir, a))) continue;
        if (!FindResourceW(nullptr, MAKEINTRESOURCEW(kArch[a].resource), RT_RCDATA)) continue;
        if (!ExtractDll(kArch[a].resource, DllPath(g_gameDir, a), err)) {
            if (a == g_arch) { SetStatus(L"No se pudo instalar el mod: " + err); return; }
        }
    }
    // Version de Steam: steam_appid.txt evita que Steam relance el juego con su opcion por defecto
    // (y asi se respeta la version 32/64 elegida). Steam sigue comprobando que tienes el juego.
    if (FileExists(g_gameDir + kArch[g_arch].dir + (g_arch ? L"\\steam_api.dll" : L"\\steam_api64.dll"))) {
        std::wstring appid = g_gameDir + kArch[g_arch].dir + L"\\steam_appid.txt";
        if (!FileExists(appid)) {
            HANDLE f = CreateFileW(appid.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD w;
                WriteFile(f, "238320", 6, &w, nullptr);
                CloseHandle(f);
            }
        }
    }
    SetStatus(g_arch ? L"Iniciando Outlast (32 bits)..." : L"Iniciando Outlast (64 bits)...");
    std::wstring exe = ExePath(g_gameDir, g_arch);
    std::wstring dir = g_gameDir + kArch[g_arch].dir;
    if ((INT_PTR)ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL) <= 32) {
        SetStatus(L"No se pudo iniciar el juego.");
        return;
    }
    SetStatus(L"Outlast iniciado. Dentro del juego: F9 = conectar / hospedar.");
    SetTimer(g_wnd, IdTimer + 1, 2500, nullptr);  // cerrar el launcher
}

static void Browse() {
    BROWSEINFOW bi{};
    bi.hwndOwner = g_wnd;
    bi.lpszTitle = L"Selecciona la carpeta de Outlast (la que contiene Binaries)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t buf[MAX_PATH];
    SHGetPathFromIDListW(pidl, buf);
    CoTaskMemFree(pidl);
    std::wstring p = Normalize(buf);
    // Aceptar también si eligen Binaries o Win64
    for (int i = 0; i < 3 && !IsGameDir(p); ++i) p = p.substr(0, p.find_last_of(L'\\'));
    if (!IsGameDir(p)) { SetStatus(L"Esa carpeta no contiene Outlast (Binaries\\Win64\\OLGame.exe)."); return; }
    g_gameDir = p;
    SavePath(p);
    PickArch();
    UpdatePathLabel();
    SetStatus(L"Ruta guardada. Pulsa Jugar.");
}

static void Uninstall() {
    bool ok = true;
    for (int a = 0; a < 2; ++a)
        if (FileExists(DllPath(g_gameDir, a)) && !DeleteFileW(DllPath(g_gameDir, a).c_str())) ok = false;
    SetStatus(ok ? L"Mod desinstalado (dinput8.dll eliminado de Win32 y Win64)."
                 : L"No se pudo eliminar dinput8.dll (¿el juego esta abierto?).");
    UpdatePathLabel();
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IdPlay: Play(); break;
        case IdBrowse: g_countdown = -1; KillTimer(h, IdTimer); Browse(); break;
        case IdUninstall: g_countdown = -1; KillTimer(h, IdTimer); Uninstall(); break;
        case IdArch:
            g_countdown = -1;
            KillTimer(h, IdTimer);
            g_arch = 1 - g_arch;
            SaveArch();
            UpdatePathLabel();
            SetStatus(g_arch ? L"Se usara la version de 32 bits. Pulsa Jugar." : L"Se usara la version de 64 bits. Pulsa Jugar.");
            break;
        }
        return 0;
    case WM_TIMER:
        if (wp == IdTimer + 1) { DestroyWindow(h); return 0; }
        if (g_countdown > 0) {
            SetStatus(std::wstring(L"Outlast encontrado. Iniciando ") + (g_arch ? L"32" : L"64") + L" bits en " +
                      std::to_wstring(g_countdown) + L"...  (pulsa cualquier boton para cancelar)");
            --g_countdown;
        } else if (g_countdown == 0) {
            Play();
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, RGB(220, 220, 220));
        return (LRESULT)GetStockObject(BLACK_BRUSH);
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmd, int) {
    CoInitialize(nullptr);
    InitCommonControls();
    bool noAuto = cmd && wcsstr(cmd, L"--setup");

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"OutlastOnlineLauncher";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    int W = 560, H = 280;
    g_wnd = CreateWindowW(wc.lpszClassName, L"Outlast Online", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                          (GetSystemMetrics(SM_CXSCREEN) - W) / 2, (GetSystemMetrics(SM_CYSCREEN) - H) / 2, W, H,
                          nullptr, nullptr, inst, nullptr);
    HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT big = CreateFontW(-26, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HWND title = CreateWindowW(L"STATIC", L"OUTLAST ONLINE", WS_CHILD | WS_VISIBLE, 20, 12, 500, 34, g_wnd, nullptr, inst, nullptr);
    g_path = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS, 20, 54, 510, 22, g_wnd, nullptr, inst, nullptr);
    g_status = CreateWindowW(L"STATIC", L"Buscando Outlast...", WS_CHILD | WS_VISIBLE, 20, 82, 510, 40, g_wnd, nullptr, inst, nullptr);
    g_btnPlay = CreateWindowW(L"BUTTON", L"Jugar", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 20, 135, 150, 36, g_wnd,
                              (HMENU)IdPlay, inst, nullptr);
    g_btnBrowse = CreateWindowW(L"BUTTON", L"Cambiar carpeta...", WS_CHILD | WS_VISIBLE, 185, 135, 170, 36, g_wnd,
                                (HMENU)IdBrowse, inst, nullptr);
    g_btnUninstall = CreateWindowW(L"BUTTON", L"Desinstalar mod", WS_CHILD | WS_VISIBLE, 370, 135, 160, 36, g_wnd,
                                   (HMENU)IdUninstall, inst, nullptr);
    g_btnArch = CreateWindowW(L"BUTTON", L"Version: 64 bits", WS_CHILD | WS_VISIBLE, 20, 182, 510, 34, g_wnd,
                              (HMENU)IdArch, inst, nullptr);
    for (HWND c : {g_path, g_status, g_btnPlay, g_btnBrowse, g_btnUninstall, g_btnArch})
        SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(title, WM_SETFONT, (WPARAM)big, TRUE);
    ShowWindow(g_wnd, SW_SHOW);
    UpdateWindow(g_wnd);

    g_gameDir = FindGame();
    if (!g_gameDir.empty()) PickArch();
    UpdatePathLabel();
    if (g_gameDir.empty()) {
        SetStatus(L"No se encontro Outlast automaticamente. Pulsa 'Cambiar carpeta...' y elige la carpeta del juego.");
    } else {
        SavePath(g_gameDir);
        if (IsGameRunning()) {
            SetStatus(L"Outlast ya esta abierto. Cierralo y pulsa Jugar para actualizar el mod.");
        } else if (noAuto) {
            SetStatus(L"Outlast encontrado. Pulsa Jugar.");
        } else {
            g_countdown = 3;
            SetTimer(g_wnd, IdTimer, 1000, nullptr);
            SendMessageW(g_wnd, WM_TIMER, IdTimer, 0);
        }
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(g_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
