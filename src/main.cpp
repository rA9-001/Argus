// ---------------------------------------------------------------------------
//  Argus - tray host, global hotkeys, process entry point.
// ---------------------------------------------------------------------------
#include "app.h"
#include "../res/resource.h"
#include <shlwapi.h>
#include <algorithm>
using std::min;

namespace {

constexpr UINT WM_TRAY       = WM_APP + 1;
constexpr UINT WM_DO_CAPTURE = WM_APP + 2;   // wParam = Grab

enum {
    HK_REGION = 1, HK_FULL, HK_WINDOW, HK_REPEAT
};

enum {
    ID_REGION = 100, ID_FULL, ID_WINDOW, ID_REPEAT,
    ID_FOLDER, ID_SETTINGS, ID_STARTUP, ID_PRTSC, ID_ABOUT, ID_EXIT
};

// What a click on the current balloon should do.
enum { BALLOON_FOLDER = 0, BALLOON_FIX_PRTSC };

constexpr WPARAM REQ_QUIT = 0xFFFF;          // sentinel for the broadcast below

HINSTANCE      g_inst = nullptr;
HWND           g_tray = nullptr;
NOTIFYICONDATAW g_nid{};
UINT           g_taskbarCreated = 0;
UINT           g_msgRequest     = 0;         // cross-instance request broadcast
int            g_balloonAction  = BALLOON_FOLDER;
std::wstring   g_hotkeyProblems;

// ---------------------------------------------------------------------------
//  Windows 11 routes PrintScreen to the Snipping Tool inside the input stack,
//  before any registered hotkey is consulted.  RegisterHotKey still reports
//  success, so the only way to know is to read the setting the Accessibility
//  page writes.  An absent value counts as enabled on current builds.
// ---------------------------------------------------------------------------
constexpr const wchar_t* kKbdKey  = L"Control Panel\\Keyboard";
constexpr const wchar_t* kSnipVal = L"PrintScreenKeyForSnippingEnabled";

bool SnippingOwnsPrintScreen() {
    HKEY  k;
    DWORD val = 1, size = sizeof(val);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKbdKey, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return true;
    if (RegQueryValueExW(k, kSnipVal, nullptr, nullptr,
                         reinterpret_cast<BYTE*>(&val), &size) != ERROR_SUCCESS)
        val = 1;
    RegCloseKey(k);
    return val != 0;
}

void SetSnippingOwnsPrintScreen(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKbdKey, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    DWORD v = on ? 1u : 0u;
    RegSetValueExW(k, kSnipVal, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&v), sizeof(v));
    RegCloseKey(k);
}

// Starts a replacement instance that waits for this one to let go of the
// single-instance mutex.  Returns false if the process could not be started,
// in which case the caller should stay alive.
bool RelaunchSelf() {
    std::wstring cmd = L"\"" + ExePath() + L"\" --restart";
    STARTUPINFOW        si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool AnyHotkeyUsesPrintScreen() {
    const std::wstring* all[] = { &g_cfg.hotkeyRegion, &g_cfg.hotkeyFull,
                                  &g_cfg.hotkeyWindow, &g_cfg.hotkeyRepeat };
    for (const std::wstring* s : all) {
        UINT mods = 0, vk = 0;
        if (ParseHotkey(*s, mods, vk) && vk == VK_SNAPSHOT) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
void AddTrayIcon() {
    g_nid = NOTIFYICONDATAW{};
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_tray;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon            = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    const std::wstring tip = std::wstring(APP_NAME) + L"  \u2022  " + g_cfg.hotkeyRegion;
    lstrcpynW(g_nid.szTip, tip.c_str(), (int)std::size(g_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

constexpr const wchar_t* kRunKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// The command the Run entry should hold while startup is enabled.
std::wstring StartupCommand() { return L"\"" + ExePath() + L"\""; }

// The command it currently holds, or empty when there is no entry.
std::wstring StartupEntry() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return L"";
    wchar_t buf[MAX_PATH * 2]{};
    DWORD   cb = sizeof(buf) - sizeof(wchar_t);   // leave room to terminate
    DWORD   type = 0;
    const LONG r = RegQueryValueExW(k, APP_NAME, nullptr, &type,
                                    reinterpret_cast<BYTE*>(buf), &cb);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || type != REG_SZ) return L"";
    return buf;
}

bool StartupEnabled() { return !StartupEntry().empty(); }

// Registry only, so the launch-time reconcile does not rewrite the ini.
void WriteStartupEntry(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    if (on) {
        const std::wstring cmd = StartupCommand();
        RegSetValueExW(k, APP_NAME, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(cmd.c_str()),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, APP_NAME);
    }
    RegCloseKey(k);
}

void SetStartup(bool on) {
    WriteStartupEntry(on);
    g_cfg.runAtStartup = on;
    SaveSettings();
}

// Make the Run entry agree with the settings, once per launch.  This is what
// registers Argus on a first run - runAtStartup defaults to 1 - and what
// repoints the entry at the new location after the exe has been moved.
void SyncStartup() {
    const std::wstring want = g_cfg.runAtStartup ? StartupCommand() : L"";
    if (StartupEntry() != want) WriteStartupEntry(g_cfg.runAtStartup);
}

// ---------------------------------------------------------------------------
struct HotkeySpec { int id; const std::wstring* text; const wchar_t* label; };

void RegisterHotkeys() {
    g_hotkeyProblems.clear();
    const HotkeySpec specs[] = {
        { HK_REGION, &g_cfg.hotkeyRegion, L"Capture region" },
        { HK_FULL,   &g_cfg.hotkeyFull,   L"Capture screen" },
        { HK_WINDOW, &g_cfg.hotkeyWindow, L"Capture window" },
        { HK_REPEAT, &g_cfg.hotkeyRepeat, L"Repeat region"  },
    };

    for (const HotkeySpec& s : specs) {
        UnregisterHotKey(g_tray, s.id);
        if (s.text->empty()) continue;

        UINT mods = 0, vk = 0;
        bool ok = ParseHotkey(*s.text, mods, vk);
        if (ok) ok = RegisterHotKey(g_tray, s.id, mods, vk) != 0;
        if (!ok) {
            if (!g_hotkeyProblems.empty()) g_hotkeyProblems += L"\n";
            g_hotkeyProblems += std::wstring(s.label) + L"  (" + *s.text + L")";
        }
    }
}

void ShowAbout() {
    const std::wstring msg =
        L"Argus " APP_VERSION L"\n\n"
        L"HOTKEYS\n"
        L"  " + g_cfg.hotkeyRegion + L"\tCapture a region\n"
        L"  " + g_cfg.hotkeyWindow + L"\tCapture the active window\n"
        L"  " + g_cfg.hotkeyFull   + L"\tCapture the current screen\n"
        L"  " + g_cfg.hotkeyRepeat + L"\tRepeat the last region\n\n"
        L"WHILE CAPTURING\n"
        L"  Drag\t\tSelect a region\n"
        L"  Click\t\tGrab the window under the cursor\n"
        L"  Ctrl+A\t\tSelect the whole screen\n"
        L"  Arrows\t\tNudge (Shift = 10px, Ctrl = resize)\n"
        L"  C\t\tCopy the colour under the cursor\n"
        L"  M\t\tToggle the magnifier\n\n"
        L"TOOLS\n"
        L"  V P L A R E H T N B\tSelect, Pen, Line, Arrow, Rect,\n"
        L"  \t\tEllipse, Highlight, Text, Number, Blur\n"
        L"  1 - 6\t\tColour     Wheel\tThickness\n"
        L"  Ctrl+Z / Ctrl+Y\tUndo / Redo\n\n"
        L"FINISH\n"
        L"  Enter\t\tCopy to clipboard\n"
        L"  Ctrl+S\t\tSave as\u2026\n"
        L"  Ctrl+Shift+S\tSave straight to the screenshots folder\n"
        L"  Esc\t\tCancel\n\n"
        L"Settings: " + ConfigPath();

    MessageBoxW(nullptr, msg.c_str(), APP_NAME L" \u2014 shortcuts",
                MB_OK | MB_ICONINFORMATION);
}

} // namespace

// ---------------------------------------------------------------------------
void Toast(const std::wstring& title, const std::wstring& msg, bool error) {
    if (!g_tray) return;
    NOTIFYICONDATAW n = g_nid;
    n.uFlags     = NIF_INFO;
    n.dwInfoFlags = error ? NIIF_ERROR : NIIF_INFO;
    g_balloonAction = BALLOON_FOLDER;            // callers may override after
    lstrcpynW(n.szInfoTitle, title.c_str(), (int)std::size(n.szInfoTitle));
    lstrcpynW(n.szInfo,      msg.c_str(),   (int)std::size(n.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// ---------------------------------------------------------------------------
namespace {

void ShowTrayMenu() {
    POINT p;
    GetCursorPos(&p);

    HMENU m = CreatePopupMenu();
    auto item = [&](UINT id, const std::wstring& text) {
        AppendMenuW(m, MF_STRING, id, text.c_str());
    };

    item(ID_REGION, L"Capture region\t"    + g_cfg.hotkeyRegion);
    item(ID_WINDOW, L"Capture window\t"    + g_cfg.hotkeyWindow);
    item(ID_FULL,   L"Capture screen\t"    + g_cfg.hotkeyFull);
    item(ID_REPEAT, L"Repeat last region\t" + g_cfg.hotkeyRepeat);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    item(ID_FOLDER,   L"Open screenshots folder");
    item(ID_SETTINGS, L"Edit settings\u2026");
    AppendMenuW(m, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0),
                ID_STARTUP, L"Run at startup");
    if (AnyHotkeyUsesPrintScreen())
        AppendMenuW(m, MF_STRING | (SnippingOwnsPrintScreen() ? 0 : MF_CHECKED),
                    ID_PRTSC, L"Give the PrintScreen key to Argus");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    item(ID_ABOUT, L"Shortcuts\u2026");
    item(ID_EXIT,  L"Exit " APP_NAME);

    SetForegroundWindow(g_tray);                 // so the menu dismisses properly
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_tray, nullptr);
    PostMessage(g_tray, WM_NULL, 0, 0);
    DestroyMenu(m);
}

void OnCommand(UINT id) {
    switch (id) {
    case ID_REGION: PostMessageW(g_tray, WM_DO_CAPTURE, (WPARAM)Grab::Region,       0); break;
    case ID_FULL:   PostMessageW(g_tray, WM_DO_CAPTURE, (WPARAM)Grab::FullScreen,   0); break;
    case ID_WINDOW: PostMessageW(g_tray, WM_DO_CAPTURE, (WPARAM)Grab::ActiveWindow, 0); break;
    case ID_REPEAT: PostMessageW(g_tray, WM_DO_CAPTURE, (WPARAM)Grab::Repeat,       0); break;

    case ID_FOLDER: {
        std::wstring dir = g_cfg.saveDir.empty() ? DefaultSaveDir() : g_cfg.saveDir;
        EnsureDir(dir);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case ID_SETTINGS: {
        SaveSettings();                          // make sure the file exists
        const std::wstring ini = ConfigPath();
        ShellExecuteW(nullptr, L"open", L"notepad.exe", ini.c_str(), nullptr, SW_SHOWNORMAL);
        break;
    }
    case ID_STARTUP: SetStartup(!StartupEnabled()); break;

    case ID_PRTSC: {
        SetSnippingOwnsPrintScreen(!SnippingOwnsPrintScreen());
        // Windows only re-decides who owns PrintScreen when the hotkey is
        // claimed by a newly started process: re-registering in place is not
        // enough (measured).  So hand over to a fresh copy of ourselves.
        if (RelaunchSelf()) DestroyWindow(g_tray);
        else Toast(APP_NAME, L"Setting changed. Restart Argus to apply it.", false);
        break;
    }

    case ID_ABOUT:   ShowAbout();                   break;
    case ID_EXIT:    DestroyWindow(g_tray);         break;
    default: break;
    }
}

LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated) { AddTrayIcon(); return 0; }

    if (msg == g_msgRequest && g_msgRequest) {          // from a second instance
        if (wp == REQ_QUIT) DestroyWindow(hwnd);
        else                PostMessageW(hwnd, WM_DO_CAPTURE, wp, 0);
        return 0;
    }

    switch (msg) {
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
            PostMessageW(hwnd, WM_DO_CAPTURE, (WPARAM)Grab::Region, 0);
            return 0;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu();
            return 0;
        case NIN_BALLOONUSERCLICK:
            OnCommand(g_balloonAction == BALLOON_FIX_PRTSC ? ID_PRTSC : ID_FOLDER);
            g_balloonAction = BALLOON_FOLDER;
            return 0;
        default:
            return 0;
        }

    case WM_HOTKEY: {
        Grab mode = Grab::Region;
        switch (wp) {
        case HK_FULL:   mode = Grab::FullScreen;   break;
        case HK_WINDOW: mode = Grab::ActiveWindow; break;
        case HK_REPEAT: mode = Grab::Repeat;       break;
        default:        mode = Grab::Region;       break;
        }
        // The overlay must not be built inside the hotkey message itself, or
        // the key that triggered it can leak through to the new window.
        PostMessageW(hwnd, WM_DO_CAPTURE, (WPARAM)mode, 0);
        return 0;
    }

    case WM_DO_CAPTURE:
        // Pressing the hotkey while a capture is genuinely on screen is a
        // no-op, on purpose.  But an overlay stranded behind another window
        // used to swallow every press from then on, until something else
        // took the foreground and released it - which is why the key would
        // "stop working" inside an app and come back after a taskbar click.
        // Recycle that one instead of dropping the key.
        if (OverlayIsStale()) OverlayAbandon();
        if (!OverlayIsActive()) OverlayShow(g_inst, (Grab)wp);
        return 0;

    case WM_COMMAND:
        OnCommand(LOWORD(wp));
        return 0;

    case WM_DESTROY:
        for (int id = HK_REGION; id <= HK_REPEAT; ++id) UnregisterHotKey(hwnd, id);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
        OverlayShutdown();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --bench: time the capture pipeline.  Results go to the parent console and
// to %TEMP%\argus-bench.txt.  Deliberately avoids <cstdio>, which would pull
// tens of kilobytes of formatting machinery into a 280 KB program.
std::wstring g_bench;

void Emit(const std::wstring& line) { g_bench += line; g_bench += L"\r\n"; }

std::wstring Fixed2(double v) {                 // 6.42  -  two decimal places
    if (v < 0) v = 0;
    const int cents = (int)(v * 100.0 + 0.5);
    wchar_t buf[32];
    wsprintfW(buf, L"%d.%02d", cents / 100, cents % 100);
    return buf;
}

std::wstring Pad(std::wstring s, int width, bool left = true) {
    while ((int)s.size() < width) { if (left) s += L' '; else s.insert(s.begin(), L' '); }
    return s;
}

struct MonitorSpec { RECT rc; wchar_t device[32]; };

BOOL CALLBACK CollectMonitor(HMONITOR h, HDC, LPRECT, LPARAM lp) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(h, &mi)) {
        MonitorSpec s{};
        s.rc = mi.rcMonitor;
        lstrcpynW(s.device, mi.szDevice, (int)std::size(s.device));
        reinterpret_cast<std::vector<MonitorSpec>*>(lp)->push_back(s);
    }
    return TRUE;
}

void RunBench() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    auto now = [] { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; };
    auto ms  = [&](LONGLONG a, LONGLONG b) {
        return (double)(b - a) * 1000.0 / (double)freq.QuadPart;
    };

    auto report = [&](const wchar_t* label, double best, double total, int n) {
        Emit(L"  " + Pad(label, 24) + L"best " + Pad(Fixed2(best), 7, false) +
             L" ms    avg " + Pad(Fixed2(total / n), 7, false) + L" ms");
    };

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring pngPath = std::wstring(tmp) + L"argus-bench.png";

    Capture cap;
    if (!CaptureScreen(cap)) { Emit(L"capture failed"); return; }
    wchar_t head[128];
    wsprintfW(head, L"\r\nArgus benchmark  -  virtual screen %d x %d\r\n", cap.w, cap.h);
    Emit(head);
    cap.reset();

    const int N = 20;
    double best = 1e9, total = 0;
    for (int i = 0; i < N; ++i) {
        const LONGLONG t0 = now();
        CaptureScreen(cap);
        const double d = ms(t0, now());
        best = min(best, d); total += d;
        if (i < N - 1) cap.reset();
    }
    report(L"full-screen capture", best, total, N);

    // --- where does that go: allocating the surface, or reading the screen? --
    {
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = vw;
        bi.bmiHeader.biHeight      = -vh;
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        best = 1e9; total = 0;
        for (int i = 0; i < N; ++i) {
            void* bits = nullptr;
            const LONGLONG t0 = now();
            HBITMAP b = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            HDC     d = CreateCompatibleDC(nullptr);
            SelectObject(d, b);
            const double dt = ms(t0, now());
            best = min(best, dt); total += dt;
            DeleteDC(d); DeleteObject(b);
        }
        report(L"  allocate surface", best, total, N);

        // Reuse one surface and time only the screen read.
        void* bits = nullptr;
        HBITMAP b = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HDC     d = CreateCompatibleDC(nullptr);
        SelectObject(d, b);

        best = 1e9; total = 0;
        for (int i = 0; i < N; ++i) {
            HDC screen = GetDC(nullptr);
            const LONGLONG t0 = now();
            BitBlt(d, 0, 0, vw, vh, screen, vx, vy, SRCCOPY);
            const double dt = ms(t0, now());
            ReleaseDC(nullptr, screen);
            best = min(best, dt); total += dt;
        }
        report(L"  read screen (reused)", best, total, N);

        // Same read, but through each monitor's own device context.
        std::vector<MonitorSpec> mons;
        EnumDisplayMonitors(nullptr, nullptr, CollectMonitor,
                            reinterpret_cast<LPARAM>(&mons));
        best = 1e9; total = 0;
        for (int i = 0; i < N; ++i) {
            const LONGLONG t0 = now();
            for (const MonitorSpec& m : mons) {
                HDC src = CreateDCW(L"DISPLAY", m.device, nullptr, nullptr);
                if (!src) continue;
                BitBlt(d, m.rc.left - vx, m.rc.top - vy,
                       m.rc.right - m.rc.left, m.rc.bottom - m.rc.top,
                       src, 0, 0, SRCCOPY);
                DeleteDC(src);
            }
            const double dt = ms(t0, now());
            best = min(best, dt); total += dt;
        }
        report(L"  read per-monitor", best, total, N);

        DeleteDC(d); DeleteObject(b);
    }

    best = 1e9; total = 0;
    for (int i = 0; i < N; ++i) {
        WindowTargets t;
        const LONGLONG t0 = now();
        CollectWindowTargets(cap, t);
        const double d = ms(t0, now());
        best = min(best, d); total += d;
    }
    report(L"window snap discovery", best, total, N);

    const RECT region{ 0, 0, min(1920, cap.w), min(1080, cap.h) };
    best = 1e9; total = 0;
    Capture crop;
    for (int i = 0; i < N; ++i) {
        crop.reset();
        const LONGLONG t0 = now();
        CaptureRegionBitmap(cap, region, crop);
        const double d = ms(t0, now());
        best = min(best, d); total += d;
    }
    report(L"crop 1920x1080", best, total, N);

    best = 1e9; total = 0;
    for (int i = 0; i < 8; ++i) {
        const LONGLONG t0 = now();
        SavePng(pngPath, crop);
        const double d = ms(t0, now());
        best = min(best, d); total += d;
    }
    report(L"PNG encode 1920x1080", best, total, 8);

    best = 1e9; total = 0;
    for (int i = 0; i < 8; ++i) {
        const LONGLONG t0 = now();
        CopyToClipboard(nullptr, crop);
        const double d = ms(t0, now());
        best = min(best, d); total += d;
    }
    report(L"clipboard 1920x1080", best, total, 8);

    crop.reset();
    cap.reset();
    DeleteFileW(pngPath.c_str());

    // Console output, if this was launched from one.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE con = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
        if (con != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteConsoleW(con, g_bench.c_str(), (DWORD)g_bench.size(), &written, nullptr);
            CloseHandle(con);
        }
        FreeConsole();
    }

    // ...and a file, because a GUI-subsystem process returns to the shell
    // prompt before the console output lands.
    const std::wstring txt = std::wstring(tmp) + L"argus-bench.txt";
    HANDLE f = CreateFileW(txt.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        const int need = WideCharToMultiByte(CP_UTF8, 0, g_bench.c_str(),
                                             (int)g_bench.size(), nullptr, 0, nullptr, nullptr);
        std::vector<char> utf8((size_t)need);
        WideCharToMultiByte(CP_UTF8, 0, g_bench.c_str(), (int)g_bench.size(),
                            utf8.data(), need, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        CloseHandle(f);
    }
}

bool HasArg(const wchar_t* needle) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    bool found = false;
    for (int i = 1; i < argc && !found; ++i)
        if (!lstrcmpiW(argv[i], needle)) found = true;
    LocalFree(argv);
    return found;
}

} // namespace

// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    g_inst = hInst;

    // --- single instance ---------------------------------------------------
    HANDLE mutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    bool alreadyRunning = (GetLastError() == ERROR_ALREADY_EXISTS);

    // A --restart copy is launched by the instance it replaces, so the old one
    // is still holding the mutex for a moment.  Wait it out rather than
    // mistaking it for a second instance.
    const bool isRestart = HasArg(L"--restart");
    for (int i = 0; isRestart && alreadyRunning && i < 60; ++i) {
        Sleep(50);
        if (mutex) CloseHandle(mutex);
        mutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
        alreadyRunning = (GetLastError() == ERROR_ALREADY_EXISTS);
    }

    g_msgRequest = RegisterWindowMessageW(L"ArgusCaptureRequest.v1");

    if (alreadyRunning) {
        // Hand the request to the instance that already owns the hotkeys.  A
        // broadcast beats FindWindow here: it needs no class-name lookup and
        // keeps working however the running instance names its window.
        WPARAM req = (WPARAM)Grab::Region;
        if      (HasArg(L"--quit"))   req = REQ_QUIT;
        else if (HasArg(L"--full"))   req = (WPARAM)Grab::FullScreen;
        else if (HasArg(L"--window")) req = (WPARAM)Grab::ActiveWindow;
        else if (HasArg(L"--repeat")) req = (WPARAM)Grab::Repeat;

        PostMessageW(HWND_BROADCAST, g_msgRequest, req, 0);
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    LoadSettings();
    g_trace = HasArg(L"--trace");

    if (HasArg(L"--bench")) {
        RunBench();
        CoUninitialize();
        if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
        return 0;
    }

    SyncStartup();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = TrayProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = WNDCLS_TRAY;
    if (!RegisterClassExW(&wc) || !OverlayRegisterClass(hInst)) return 1;

    // A hidden top-level window, not HWND_MESSAGE: message-only windows are
    // invisible to FindWindow and never receive the TaskbarCreated broadcast.
    g_tray = CreateWindowExW(WS_EX_TOOLWINDOW, WNDCLS_TRAY, APP_NAME, WS_POPUP,
                             0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!g_tray) return 1;

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();
    RegisterHotkeys();

    if (!g_hotkeyProblems.empty()) {
        Toast(L"Hotkey unavailable",
              L"Another app already owns:\n" + g_hotkeyProblems +
              L"\n\nRight-click the tray icon \u2192 Edit settings to pick another.",
              true);
    } else if (AnyHotkeyUsesPrintScreen() && SnippingOwnsPrintScreen()) {
        // RegisterHotKey reports success, but Windows still eats the key.
        Toast(L"Windows is using PrintScreen",
              L"That key opens the Snipping Tool. Click here to give it to Argus, "
              L"or use the tray menu.", false);
        g_balloonAction = BALLOON_FIX_PRTSC;
    } else if (isRestart) {
        // Handing the key back is not symmetric: Windows arms its PrintScreen
        // handler at sign-in, so it only reclaims the key on the next logon.
        Toast(APP_NAME,
              SnippingOwnsPrintScreen()
                  ? L"PrintScreen released. Windows takes it back after you "
                    L"next sign in."
                  : L"PrintScreen now opens Argus.", false);
    } else if (g_firstRun && g_cfg.runAtStartup) {
        Toast(APP_NAME,
              L"Running in the tray, and set to start with Windows.\n"
              L"Right-click the icon to turn that off.", false);
    }

    // Launching with an explicit mode captures immediately.
    if      (HasArg(L"--region")) PostMessageW(g_tray, WM_DO_CAPTURE, (WPARAM)Grab::Region,       0);
    else if (HasArg(L"--full"))   PostMessageW(g_tray, WM_DO_CAPTURE, (WPARAM)Grab::FullScreen,   0);
    else if (HasArg(L"--window")) PostMessageW(g_tray, WM_DO_CAPTURE, (WPARAM)Grab::ActiveWindow, 0);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return (int)msg.wParam;
}
