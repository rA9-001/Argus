// ---------------------------------------------------------------------------
//  Argus - a lightweight screenshot tool.  Shared declarations.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <vector>
#include <string>
#include <cstdint>

#define APP_NAME       L"Argus"
#define APP_VERSION    L"1.0"
#define WNDCLS_OVERLAY L"ArgusOverlay"
#define WNDCLS_TRAY    L"ArgusTray"
#define MUTEX_NAME     L"Local\\Argus.SingleInstance.v1"

// ---------------------------------------------------------------------------
//  Captured screen bits.  Client coordinates of the overlay are identical to
//  pixel coordinates in this bitmap, so the two are used interchangeably.
// ---------------------------------------------------------------------------
struct Capture {
    HBITMAP   bmp = nullptr;   // 32bpp top-down DIB section
    uint32_t* px  = nullptr;   // 0xAARRGGBB little-endian (i.e. BGRA bytes)
    HDC       dc  = nullptr;   // memory DC with bmp selected
    int       w = 0, h = 0;
    int       ox = 0, oy = 0;  // virtual-screen origin, may be negative

    bool valid() const { return bmp != nullptr; }
    void reset();
    // 0x00RRGGBB.  Works whether or not the surface is CPU-readable.
    uint32_t at(int x, int y) const;
};

bool CaptureScreen(Capture& c);
bool CaptureRegionBitmap(const Capture& src, const RECT& r, Capture& out);

// Snap targets: visible window frames (front-to-back) plus their immediate
// child panes, clipped to the virtual screen and expressed in capture pixels.
struct WindowTargets {
    std::vector<RECT>              frames;
    std::vector<std::vector<RECT>> panes;   // parallel to frames
    void clear() { frames.clear(); panes.clear(); }
    bool hit(POINT p, RECT& out) const;
};
void CollectWindowTargets(const Capture& c, WindowTargets& out);
bool WindowRectOf(const Capture& c, HWND hwnd, RECT& out);
bool MonitorRectAt(const Capture& c, POINT ptClient, RECT& out);

// ---------------------------------------------------------------------------
//  Annotations
// ---------------------------------------------------------------------------
enum class Tool {
    Select = 0, Pen, Line, Arrow, Rect, Ellipse, Highlight, Text, Counter, Pixelate,
    COUNT
};

struct Shape {
    Tool                 tool  = Tool::Pen;
    COLORREF             color = RGB(255, 61, 61);
    int                  width = 3;
    bool                 filled = false;
    std::vector<POINT>   pts;      // Pen: polyline.  Others: [0]=start [1]=end.
    std::wstring         text;     // Text
    int                  number = 0;  // Counter
};

// ---------------------------------------------------------------------------
//  Settings  (%APPDATA%\Argus\argus.ini)
// ---------------------------------------------------------------------------
struct Settings {
    std::wstring hotkeyRegion  = L"PrintScreen";
    std::wstring hotkeyFull    = L"Shift+PrintScreen";
    std::wstring hotkeyWindow  = L"Ctrl+PrintScreen";
    std::wstring hotkeyRepeat  = L"Ctrl+Shift+PrintScreen";
    std::wstring saveDir;                 // empty -> Pictures\Screenshots
    bool  copyOnSave     = true;          // also place on clipboard when saving
    bool  quickSave      = false;         // Ctrl+S skips the file dialog
    bool  playSound      = false;
    bool  showMagnifier  = true;
    bool  runAtStartup   = false;
    int   dimAlpha       = 110;           // 0..255
    COLORREF accent      = RGB(76, 141, 255);
};

extern Settings g_cfg;
void LoadSettings();
void SaveSettings();
std::wstring ConfigPath();
std::wstring DefaultSaveDir();
bool ParseHotkey(const std::wstring& s, UINT& mods, UINT& vk);

// ---------------------------------------------------------------------------
//  Output
// ---------------------------------------------------------------------------
bool CopyToClipboard(HWND owner, const Capture& img);
bool SavePng(const std::wstring& path, const Capture& img);
bool SaveWithDialog(HWND owner, const Capture& img, std::wstring& pathOut);
std::wstring BuildSavePath();
bool CopyTextToClipboard(HWND owner, const std::wstring& text);
void RevealInExplorer(const std::wstring& path);

// ---------------------------------------------------------------------------
//  Overlay
// ---------------------------------------------------------------------------
enum class Grab { Region, FullScreen, ActiveWindow, Repeat };

bool  OverlayRegisterClass(HINSTANCE hInst);
void  OverlayShow(HINSTANCE hInst, Grab mode);
bool  OverlayIsActive();
void  OverlayShutdown();

// ---------------------------------------------------------------------------
//  Small helpers
// ---------------------------------------------------------------------------
void        ForceForeground(HWND hwnd);
std::wstring ExePath();
std::wstring Timestamp();
void        EnsureDir(const std::wstring& dir);
void        Toast(const std::wstring& title, const std::wstring& msg, bool error = false);

// --trace: latency instrumentation, written to %TEMP%\argus-trace.txt.
extern bool g_trace;
void        TraceLine(const std::wstring& line);
long long   TraceNow();
double      TraceMs(long long from, long long to);

inline RECT Normalized(POINT a, POINT b) {
    RECT r;
    r.left   = a.x < b.x ? a.x : b.x;
    r.top    = a.y < b.y ? a.y : b.y;
    r.right  = a.x > b.x ? a.x : b.x;
    r.bottom = a.y > b.y ? a.y : b.y;
    return r;
}
inline int  RectW(const RECT& r) { return r.right  - r.left; }
inline int  RectH(const RECT& r) { return r.bottom - r.top;  }
inline bool RectEmptyish(const RECT& r) { return RectW(r) < 1 || RectH(r) < 1; }
inline int  Clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
