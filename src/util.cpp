// ---------------------------------------------------------------------------
//  Paths, settings, hotkey parsing and other odds and ends.
// ---------------------------------------------------------------------------
#include "app.h"
#include <shlobj.h>
#include <shlwapi.h>

Settings g_cfg;

// ---------------------------------------------------------------------------
std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    return std::wstring(buf, n);
}

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
    if (p) CoTaskMemFree(p);
    return out;
}

void EnsureDir(const std::wstring& dir) {
    if (!dir.empty()) SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
}

std::wstring ConfigPath() {
    std::wstring dir = KnownFolder(FOLDERID_RoamingAppData);
    if (dir.empty()) return L"";
    dir += L"\\Argus";
    EnsureDir(dir);
    return dir + L"\\argus.ini";
}

std::wstring DefaultSaveDir() {
    std::wstring pics = KnownFolder(FOLDERID_Pictures);
    if (pics.empty()) pics = KnownFolder(FOLDERID_Desktop);
    if (pics.empty()) return L"";
    return pics + L"\\Screenshots";
}

std::wstring Timestamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[64];
    wsprintfW(buf, L"%04d-%02d-%02d %02d.%02d.%02d",
              t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

// ---------------------------------------------------------------------------
//  Settings
// ---------------------------------------------------------------------------
namespace {

std::wstring IniGet(const wchar_t* sec, const wchar_t* key,
                    const std::wstring& def, const std::wstring& file) {
    wchar_t buf[1024];
    GetPrivateProfileStringW(sec, key, def.c_str(), buf, (DWORD)std::size(buf), file.c_str());
    return buf;
}

int IniGetInt(const wchar_t* sec, const wchar_t* key, int def, const std::wstring& file) {
    return (int)GetPrivateProfileIntW(sec, key, def, file.c_str());
}

void IniSet(const wchar_t* sec, const wchar_t* key, const std::wstring& val,
            const std::wstring& file) {
    WritePrivateProfileStringW(sec, key, val.c_str(), file.c_str());
}

void IniSetInt(const wchar_t* sec, const wchar_t* key, int val, const std::wstring& file) {
    wchar_t buf[24];
    wsprintfW(buf, L"%d", val);
    WritePrivateProfileStringW(sec, key, buf, file.c_str());
}

} // namespace

// Written verbatim on first run so the options are discoverable and
// commented; WritePrivateProfileString cannot emit comments itself.
static const char kDefaultIni[] =
    "; Argus settings.  Restart Argus after editing this file.\r\n"
    ";\r\n"
    "; Hotkeys are written as  Ctrl / Alt / Shift / Win  plus one key, e.g.\r\n"
    ";   PrintScreen | Ctrl+Shift+4 | Alt+F1 | Win+S\r\n"
    "; Key names: PrintScreen, F1-F24, A-Z, 0-9, Insert, Delete, Home, End,\r\n"
    ";   PageUp, PageDown, Space, Tab, Esc, Enter, Left, Right, Up, Down.\r\n"
    "; Leave a value empty to disable that hotkey.\r\n"
    "\r\n"
    "[hotkeys]\r\n"
    "region=PrintScreen\r\n"
    "window=Ctrl+PrintScreen\r\n"
    "full=Shift+PrintScreen\r\n"
    "repeat=Ctrl+Shift+PrintScreen\r\n"
    "\r\n"
    "[output]\r\n"
    "; Folder for quick-saves.  Empty means Pictures\\Screenshots.\r\n"
    "dir=\r\n"
    "; Also put the image on the clipboard when saving.\r\n"
    "copyOnSave=1\r\n"
    "; Make Ctrl+S skip the file dialog and save straight to the folder above.\r\n"
    "quickSave=0\r\n"
    "; Play a short beep on copy and save.\r\n"
    "sound=0\r\n"
    "\r\n"
    "[ui]\r\n"
    "; Show the pixel magnifier with the colour readout.\r\n"
    "magnifier=1\r\n"
    "; How strongly the area outside the selection is dimmed, 0-240.\r\n"
    "dim=110\r\n"
    "; Accent colour used for the selection border and highlights.\r\n"
    "accentR=76\r\n"
    "accentG=141\r\n"
    "accentB=255\r\n"
    "\r\n"
    "[app]\r\n"
    "; Start with Windows.  Kept in sync with the HKCU Run entry on every\r\n"
    "; launch, so moving argus.exe is enough to fix the shortcut.\r\n"
    "runAtStartup=1\r\n";

bool g_firstRun = false;

// True when it created the file, i.e. this is the very first run.
static bool WriteDefaultConfig(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false; // already there, or unwritable
    DWORD written = 0;
    WriteFile(f, kDefaultIni, (DWORD)(sizeof(kDefaultIni) - 1), &written, nullptr);
    CloseHandle(f);
    return true;
}

void LoadSettings() {
    const std::wstring f = ConfigPath();
    if (f.empty()) return;
    g_firstRun = WriteDefaultConfig(f);

    g_cfg.hotkeyRegion = IniGet(L"hotkeys", L"region",  g_cfg.hotkeyRegion, f);
    g_cfg.hotkeyFull   = IniGet(L"hotkeys", L"full",    g_cfg.hotkeyFull,   f);
    g_cfg.hotkeyWindow = IniGet(L"hotkeys", L"window",  g_cfg.hotkeyWindow, f);
    g_cfg.hotkeyRepeat = IniGet(L"hotkeys", L"repeat",  g_cfg.hotkeyRepeat, f);

    g_cfg.saveDir      = IniGet(L"output", L"dir", L"", f);
    g_cfg.copyOnSave   = IniGetInt(L"output", L"copyOnSave", 1, f) != 0;
    g_cfg.quickSave    = IniGetInt(L"output", L"quickSave",  0, f) != 0;
    g_cfg.playSound    = IniGetInt(L"output", L"sound",      0, f) != 0;

    g_cfg.showMagnifier = IniGetInt(L"ui", L"magnifier", 1,   f) != 0;
    g_cfg.dimAlpha      = Clampi(IniGetInt(L"ui", L"dim", 110, f), 0, 240);

    const int r = Clampi(IniGetInt(L"ui", L"accentR", 76,  f), 0, 255);
    const int g = Clampi(IniGetInt(L"ui", L"accentG", 141, f), 0, 255);
    const int b = Clampi(IniGetInt(L"ui", L"accentB", 255, f), 0, 255);
    g_cfg.accent = RGB(r, g, b);

    g_cfg.runAtStartup = IniGetInt(L"app", L"runAtStartup", 1, f) != 0;
}

void SaveSettings() {
    const std::wstring f = ConfigPath();
    if (f.empty()) return;

    IniSet(L"hotkeys", L"region", g_cfg.hotkeyRegion, f);
    IniSet(L"hotkeys", L"full",   g_cfg.hotkeyFull,   f);
    IniSet(L"hotkeys", L"window", g_cfg.hotkeyWindow, f);
    IniSet(L"hotkeys", L"repeat", g_cfg.hotkeyRepeat, f);

    IniSet(L"output", L"dir", g_cfg.saveDir, f);
    IniSetInt(L"output", L"copyOnSave", g_cfg.copyOnSave, f);
    IniSetInt(L"output", L"quickSave",  g_cfg.quickSave,  f);
    IniSetInt(L"output", L"sound",      g_cfg.playSound,  f);

    IniSetInt(L"ui", L"magnifier", g_cfg.showMagnifier, f);
    IniSetInt(L"ui", L"dim",       g_cfg.dimAlpha,      f);
    IniSetInt(L"ui", L"accentR",   GetRValue(g_cfg.accent), f);
    IniSetInt(L"ui", L"accentG",   GetGValue(g_cfg.accent), f);
    IniSetInt(L"ui", L"accentB",   GetBValue(g_cfg.accent), f);

    IniSetInt(L"app", L"runAtStartup", g_cfg.runAtStartup, f);
}

// ---------------------------------------------------------------------------
//  Hotkey strings, e.g.  "Ctrl+Shift+PrintScreen",  "Alt+A",  "F9"
// ---------------------------------------------------------------------------
namespace {

struct NamedKey { const wchar_t* name; UINT vk; };

const NamedKey kKeys[] = {
    { L"printscreen", VK_SNAPSHOT }, { L"prtsc",   VK_SNAPSHOT },
    { L"prntscrn",    VK_SNAPSHOT }, { L"prtscr",  VK_SNAPSHOT },
    { L"insert",      VK_INSERT   }, { L"ins",     VK_INSERT   },
    { L"delete",      VK_DELETE   }, { L"del",     VK_DELETE   },
    { L"home",        VK_HOME     }, { L"end",     VK_END      },
    { L"pageup",      VK_PRIOR    }, { L"pgup",    VK_PRIOR    },
    { L"pagedown",    VK_NEXT     }, { L"pgdn",    VK_NEXT     },
    { L"space",       VK_SPACE    }, { L"tab",     VK_TAB      },
    { L"escape",      VK_ESCAPE   }, { L"esc",     VK_ESCAPE   },
    { L"enter",       VK_RETURN   }, { L"return",  VK_RETURN   },
    { L"backspace",   VK_BACK     }, { L"pause",   VK_PAUSE    },
    { L"scrolllock",  VK_SCROLL   },
    { L"left",        VK_LEFT     }, { L"right",   VK_RIGHT    },
    { L"up",          VK_UP       }, { L"down",    VK_DOWN     },
    { L"comma",       VK_OEM_COMMA}, { L"period",  VK_OEM_PERIOD },
    { L"grave",       VK_OEM_3    }, { L"tilde",   VK_OEM_3    },
};

std::wstring LowerTrim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t");
    std::wstring t = s.substr(a, b - a + 1);
    for (wchar_t& ch : t) ch = (wchar_t)towlower(ch);
    return t;
}

} // namespace

bool ParseHotkey(const std::wstring& spec, UINT& mods, UINT& vk) {
    mods = 0;
    vk   = 0;

    std::wstring cur;
    std::vector<std::wstring> parts;
    for (wchar_t ch : spec) {
        if (ch == 43 || ch == 45) { parts.push_back(cur); cur.clear(); }  // + -
        else cur += ch;
    }
    parts.push_back(cur);

    for (const std::wstring& raw : parts) {
        const std::wstring p = LowerTrim(raw);
        if (p.empty()) continue;

        if (p == L"ctrl" || p == L"control") { mods |= MOD_CONTROL; continue; }
        if (p == L"alt")                     { mods |= MOD_ALT;     continue; }
        if (p == L"shift")                   { mods |= MOD_SHIFT;   continue; }
        if (p == L"win" || p == L"super")    { mods |= MOD_WIN;     continue; }

        bool found = false;
        for (const NamedKey& k : kKeys)
            if (p == k.name) { vk = k.vk; found = true; break; }
        if (found) continue;

        if (p.size() >= 2 && p[0] == L'f') {                 // F1 - F24
            const int n = _wtoi(p.c_str() + 1);
            if (n >= 1 && n <= 24) { vk = (UINT)(VK_F1 + n - 1); continue; }
        }
        if (p.size() == 1) {
            const wchar_t ch = p[0];
            if (ch >= L'a' && ch <= L'z') { vk = (UINT)(L'A' + (ch - L'a')); continue; }
            if (ch >= L'0' && ch <= L'9') { vk = (UINT)ch;                   continue; }
        }
        return false;                                        // unrecognised token
    }

    if (!vk) return false;
    mods |= MOD_NOREPEAT;
    return true;
}

// ---------------------------------------------------------------------------
//  Foreground activation.  The process owning a pressed hotkey is normally
//  granted foreground rights; attaching to the current foreground thread makes
//  this reliable across the remaining edge cases.
// ---------------------------------------------------------------------------
void ForceForeground(HWND hwnd) {
    if (!hwnd) return;

    HWND  fg      = GetForegroundWindow();
    DWORD fgTid   = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD selfTid = GetCurrentThreadId();

    if (fgTid && fgTid != selfTid) AttachThreadInput(selfTid, fgTid, TRUE);

    ShowWindow(hwnd, SW_SHOW);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);

    if (fgTid && fgTid != selfTid) AttachThreadInput(selfTid, fgTid, FALSE);
}

// ---------------------------------------------------------------------------
//  --trace  latency instrumentation
// ---------------------------------------------------------------------------
bool g_trace = false;

long long TraceNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double TraceMs(long long from, long long to) {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    return (double)(to - from) * 1000.0 / (double)freq.QuadPart;
}

void TraceLine(const std::wstring& line) {
    if (!g_trace) return;

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"argus-trace.txt";

    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;

    const std::wstring text = line + L"\r\n";
    const int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                         nullptr, 0, nullptr, nullptr);
    std::vector<char> utf8((size_t)need);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                        utf8.data(), need, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(f);
}
