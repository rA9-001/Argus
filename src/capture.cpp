// ---------------------------------------------------------------------------
//  Screen capture + window snap-target discovery.
// ---------------------------------------------------------------------------
#include "app.h"
#include <dwmapi.h>

void Capture::reset() {
    if (dc)  { DeleteDC(dc);         dc  = nullptr; }
    if (bmp) { DeleteObject(bmp);    bmp = nullptr; }
    px = nullptr;
    w = h = ox = oy = 0;
}

uint32_t Capture::at(int x, int y) const {
    if (x < 0 || y < 0 || x >= w || y >= h || !px) return 0;
    return px[(size_t)y * w + x];
}

// Allocates a top-down 32bpp DIB section and a memory DC holding it.
// A device-dependent bitmap was measured here too and came out slightly
// slower: GDI reads the screen back into host memory either way, so the DIB
// wins by also giving direct pixel access for the magnifier and encoders.
static bool MakeSurface(Capture& c, int w, int h) {
    if (w <= 0 || h <= 0) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;          // negative => top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); return false; }

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) { DeleteObject(bmp); return false; }
    SelectObject(dc, bmp);

    c.bmp = bmp;
    c.dc  = dc;
    c.px  = static_cast<uint32_t*>(bits);
    c.w   = w;
    c.h   = h;
    return true;
}

bool CaptureScreen(Capture& c) {
    c.reset();

    const int ox = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int oy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int h  = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (!MakeSurface(c, w, h)) return false;
    c.ox = ox;
    c.oy = oy;

    HDC screen = GetDC(nullptr);
    if (!screen) { c.reset(); return false; }

    // DWM composites layered/transparent windows into the screen DC already,
    // so a plain SRCCOPY is both correct and the fastest path available.
    const BOOL ok = BitBlt(c.dc, 0, 0, w, h, screen, ox, oy, SRCCOPY);
    ReleaseDC(nullptr, screen);

    if (!ok) { c.reset(); return false; }

    // Note: BitBlt leaves the alpha byte at zero.  Fixing it up here would
    // mean touching every pixel of the virtual desktop on the hotkey path;
    // only exported crops need opaque alpha, so it happens there instead.
    return true;
}

bool CaptureRegionBitmap(const Capture& src, const RECT& r, Capture& out) {
    out.reset();
    RECT s = r;
    s.left   = Clampi(s.left,   0, src.w);
    s.top    = Clampi(s.top,    0, src.h);
    s.right  = Clampi(s.right,  0, src.w);
    s.bottom = Clampi(s.bottom, 0, src.h);
    if (RectEmptyish(s)) return false;

    if (!MakeSurface(out, RectW(s), RectH(s))) return false;
    out.ox = src.ox + s.left;
    out.oy = src.oy + s.top;
    BitBlt(out.dc, 0, 0, out.w, out.h, src.dc, s.left, s.top, SRCCOPY);

    uint32_t* p   = out.px;
    uint32_t* end = p + (size_t)out.w * out.h;
    for (; p != end; ++p) *p |= 0xFF000000u;      // screenshots are opaque
    return true;
}

// ---------------------------------------------------------------------------
//  Snap targets
// ---------------------------------------------------------------------------
namespace {

struct EnumCtx {
    const Capture* cap;
    WindowTargets* out;
    DWORD          selfPid;
};

bool IsCloaked(HWND hwnd) {
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return cloaked != 0;
    return false;
}

// The DWM frame bounds exclude the invisible resize border that GetWindowRect
// reports, which is what the user actually sees.
bool FrameBounds(HWND hwnd, RECT& r) {
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
        return true;
    return GetWindowRect(hwnd, &r) != 0;
}

bool ToCaptureRect(const Capture& c, RECT screen, RECT& out) {
    RECT bounds{ c.ox, c.oy, c.ox + c.w, c.oy + c.h };
    RECT hit;
    if (!IntersectRect(&hit, &screen, &bounds)) return false;
    out.left   = hit.left   - c.ox;
    out.top    = hit.top    - c.oy;
    out.right  = hit.right  - c.ox;
    out.bottom = hit.bottom - c.oy;
    return RectW(out) >= 8 && RectH(out) >= 8;
}

void CollectPanes(const Capture& c, HWND parent, std::vector<RECT>& out) {
    // One level of direct children is enough to expose useful panes without
    // drowning the hit test in individual buttons.
    for (HWND ch = GetWindow(parent, GW_CHILD); ch; ch = GetWindow(ch, GW_HWNDNEXT)) {
        if (!IsWindowVisible(ch)) continue;
        RECT sr;
        if (!GetWindowRect(ch, &sr)) continue;
        RECT cr;
        if (!ToCaptureRect(c, sr, cr)) continue;
        if (RectW(cr) < 48 || RectH(cr) < 48) continue;
        out.push_back(cr);
        if (out.size() >= 64) break;
    }
}

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp) {
    EnumCtx* ctx = reinterpret_cast<EnumCtx*>(lp);

    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    if (IsCloaked(hwnd))                          return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->selfPid) return TRUE;          // never snap to our overlay

    const LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TRANSPARENT) return TRUE;

    wchar_t cls[64];
    if (GetClassNameW(hwnd, cls, 64) &&
        (!lstrcmpiW(cls, L"Progman") || !lstrcmpiW(cls, L"WorkerW")))
        return TRUE;                               // desktop backdrop

    RECT sr;
    if (!FrameBounds(hwnd, sr)) return TRUE;
    RECT cr;
    if (!ToCaptureRect(*ctx->cap, sr, cr)) return TRUE;

    ctx->out->frames.push_back(cr);
    ctx->out->panes.emplace_back();
    CollectPanes(*ctx->cap, hwnd, ctx->out->panes.back());

    return ctx->out->frames.size() < 200;
}

} // namespace

void CollectWindowTargets(const Capture& c, WindowTargets& out) {
    out.clear();
    EnumCtx ctx{ &c, &out, GetCurrentProcessId() };
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));   // front-to-back
}

bool WindowTargets::hit(POINT p, RECT& out) const {
    for (size_t i = 0; i < frames.size(); ++i) {
        const RECT& f = frames[i];
        if (p.x < f.left || p.x >= f.right || p.y < f.top || p.y >= f.bottom)
            continue;

        // Prefer the smallest pane of this window that contains the point.
        const RECT* best = &f;
        long bestArea = (long)RectW(f) * RectH(f);
        for (const RECT& pane : panes[i]) {
            if (p.x < pane.left || p.x >= pane.right ||
                p.y < pane.top  || p.y >= pane.bottom) continue;
            const long area = (long)RectW(pane) * RectH(pane);
            // Ignore panes that are basically the whole window.
            if (area < bestArea && area > 4000) { best = &pane; bestArea = area; }
        }
        out = *best;
        return true;
    }
    return false;
}

bool WindowRectOf(const Capture& c, HWND hwnd, RECT& out) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    RECT sr;
    if (!FrameBounds(hwnd, sr)) return false;
    return ToCaptureRect(c, sr, out);
}

bool MonitorRectAt(const Capture& c, POINT ptClient, RECT& out) {
    POINT screen{ ptClient.x + c.ox, ptClient.y + c.oy };
    HMONITOR mon = MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST);
    if (!mon) return false;
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return false;
    return ToCaptureRect(c, mi.rcMonitor, out);
}
