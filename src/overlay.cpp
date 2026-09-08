// ---------------------------------------------------------------------------
//  The capture overlay: selection, annotation input, and the commands that
//  turn a selection into a file or a clipboard entry.
// ---------------------------------------------------------------------------
#include "overlay.h"

#include <algorithm>
#include <utility>
using std::min;
using std::max;

Overlay g_ov;

namespace {

RECT s_lastSel{};
bool s_haveLastSel      = false;
bool s_suppressDeactivate = false;   // set while a modal dialog is up

// Timer ids on the overlay window.
constexpr UINT_PTR kTimerCaret    = 1;
constexpr UINT_PTR kTimerOrphan   = 2;   // see OverlayShow
constexpr UINT     kOrphanCheckMs = 1200;

int Sc(int v) { return max(1, (int)(v * g_ov.scale + 0.5f)); }

// GetDpiForMonitor lives in Shcore.dll; bind it lazily so the import table
// stays clean on systems that predate it.
float MonitorScaleAt(POINT screenPt) {
    using PFN = HRESULT (WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static PFN fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (HMODULE m = LoadLibraryW(L"Shcore.dll"))
            fn = reinterpret_cast<PFN>((void*)GetProcAddress(m, "GetDpiForMonitor"));
    }
    HMONITOR mon = MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST);
    if (fn && mon) {
        UINT dx = 96, dy = 96;
        if (SUCCEEDED(fn(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy)) && dx)
            return (float)dx / 96.0f;
    }
    return 1.0f;
}

void SyncMonitor() {
    Overlay& ov = g_ov;
    POINT scr{ ov.cursor.x + ov.cap.ox, ov.cursor.y + ov.cap.oy };
    ov.scale = MonitorScaleAt(scr);
    RECT mon;
    if (MonitorRectAt(ov.cap, ov.cursor, mon)) ov.curMonitor = mon;
}

Grip HitGrip(POINT p) {
    const Overlay& ov = g_ov;
    if (!ov.hasSel || ov.phase != Phase::Ready) return Grip::None;

    const int m = Sc(8);
    if (p.x < ov.sel.left - m || p.x > ov.sel.right + m ||
        p.y < ov.sel.top  - m || p.y > ov.sel.bottom + m) return Grip::None;

    const bool L = abs(p.x - ov.sel.left)   <= m;
    const bool R = abs(p.x - ov.sel.right)  <= m;
    const bool T = abs(p.y - ov.sel.top)    <= m;
    const bool B = abs(p.y - ov.sel.bottom) <= m;

    if (T && L) return Grip::NW;
    if (T && R) return Grip::NE;
    if (B && L) return Grip::SW;
    if (B && R) return Grip::SE;
    if (T) return Grip::N;
    if (B) return Grip::S;
    if (L) return Grip::W;
    if (R) return Grip::E;
    return PtInRect(&ov.sel, p) ? Grip::Inside : Grip::None;
}

LPCWSTR CursorFor(Grip g) {
    switch (g) {
    case Grip::N: case Grip::S:   return IDC_SIZENS;
    case Grip::E: case Grip::W:   return IDC_SIZEWE;
    case Grip::NE: case Grip::SW: return IDC_SIZENESW;
    case Grip::NW: case Grip::SE: return IDC_SIZENWSE;
    case Grip::Inside:            return IDC_SIZEALL;
    default:                      return IDC_CROSS;
    }
}

void ClampSel() {
    Overlay& ov = g_ov;
    ov.sel.left   = Clampi(ov.sel.left,   0, ov.cap.w);
    ov.sel.top    = Clampi(ov.sel.top,    0, ov.cap.h);
    ov.sel.right  = Clampi(ov.sel.right,  0, ov.cap.w);
    ov.sel.bottom = Clampi(ov.sel.bottom, 0, ov.cap.h);
    if (ov.sel.right  < ov.sel.left) ov.sel.right  = ov.sel.left;
    if (ov.sel.bottom < ov.sel.top)  ov.sel.bottom = ov.sel.top;
}

} // namespace

// ---------------------------------------------------------------------------
void Overlay::reset() {
    cap.reset();
    targets.clear();
    shapes.clear();
    undone.clear();
    buttons.clear();
    live = Shape{};
    SetRectEmpty(&sel);
    SetRectEmpty(&snap);
    SetRectEmpty(&toolbar);
    SetRectEmpty(&lastChrome);
    hasSel = hasSnap = false;
    phase = Phase::Idle;
    grip = hotGrip = Grip::None;
    mouseDown = moved = drawing = editing = false;
    toolbarVisible = false;
    hotCmd = pressedCmd = Cmd::None;
    counter = 1;
    editIndex = 0;

    if (bbDC)  { DeleteDC(bbDC);         bbDC  = nullptr; }
    if (bbBmp) { DeleteObject(bbBmp);    bbBmp = nullptr; }
    bbPx = nullptr;
    bbW = bbH = 0;
}

void OverlayDamage(const RECT& r) {
    if (!g_ov.hwnd || RectEmptyish(r)) return;
    RECT c = r;
    RECT screen{ 0, 0, g_ov.cap.w, g_ov.cap.h };
    if (IntersectRect(&c, &c, &screen)) InvalidateRect(g_ov.hwnd, &c, FALSE);
}

// Repaint everything the chrome could have touched, before and after a change.
void OverlayRefreshChrome() {
    Overlay& ov = g_ov;
    const RECT now = ChromeBounds();
    OverlayDamage(ov.lastChrome);
    OverlayDamage(now);
    ov.lastChrome = now;
}

// ---------------------------------------------------------------------------
//  Commands
// ---------------------------------------------------------------------------
namespace {

const COLORREF kPalette[6] = {
    RGB(255,  61,  61), RGB(255, 173,  51), RGB( 60, 210, 120),
    RGB( 76, 141, 255), RGB( 24,  25,  30), RGB(255, 255, 255)
};

void CloseOverlay() {
    if (g_ov.hasSel) { s_lastSel = g_ov.sel; s_haveLastSel = true; }
    if (g_ov.hwnd) DestroyWindow(g_ov.hwnd);
}

void EndTextEdit(bool keep) {
    Overlay& ov = g_ov;
    if (!ov.editing) return;
    ov.editing = false;
    KillTimer(ov.hwnd, kTimerCaret);

    if (ov.editIndex < ov.shapes.size()) {
        if (!keep || ov.shapes[ov.editIndex].text.empty())
            ov.shapes.erase(ov.shapes.begin() + (ptrdiff_t)ov.editIndex);
    }
    InvalidateRect(ov.hwnd, nullptr, FALSE);
}

bool DoCopy() {
    Overlay& ov = g_ov;
    if (!ov.hasSel || RectEmptyish(ov.sel)) return false;
    EndTextEdit(true);

    Capture out;
    if (!RenderExport(ov.sel, out)) return false;
    const bool ok = CopyToClipboard(ov.hwnd, out);
    out.reset();
    if (ok && g_cfg.playSound) MessageBeep(MB_OK);
    return ok;
}

void DoSave(bool quick) {
    Overlay& ov = g_ov;
    if (!ov.hasSel || RectEmptyish(ov.sel)) return;
    EndTextEdit(true);

    Capture out;
    if (!RenderExport(ov.sel, out)) return;

    std::wstring path;
    bool ok = false;

    if (quick || g_cfg.quickSave) {
        path = BuildSavePath();
        ok   = !path.empty() && SavePng(path, out);
    } else {
        s_suppressDeactivate = true;
        ShowWindow(ov.hwnd, SW_HIDE);            // keep the dialog unobstructed
        ok = SaveWithDialog(ov.hwnd, out, path);
        s_suppressDeactivate = false;
        if (!ok) {                               // cancelled - carry on editing
            ShowWindow(ov.hwnd, SW_SHOW);
            ForceForeground(ov.hwnd);
            out.reset();
            return;
        }
    }

    if (ok && g_cfg.copyOnSave) CopyToClipboard(ov.hwnd, out);
    out.reset();

    if (ok) {
        if (g_cfg.playSound) MessageBeep(MB_OK);
        Toast(L"Saved", path, false);
    } else {
        Toast(L"Could not save", path.empty() ? L"No writable location." : path, true);
    }
    CloseOverlay();
}

void Undo() {
    Overlay& ov = g_ov;
    if (ov.shapes.empty()) return;
    ov.undone.push_back(ov.shapes.back());
    ov.shapes.pop_back();
    InvalidateRect(ov.hwnd, nullptr, FALSE);
}

void Redo() {
    Overlay& ov = g_ov;
    if (ov.undone.empty()) return;
    ov.shapes.push_back(ov.undone.back());
    ov.undone.pop_back();
    InvalidateRect(ov.hwnd, nullptr, FALSE);
}

void SetTool(Tool t) {
    Overlay& ov = g_ov;
    if (ov.tool == t) return;
    EndTextEdit(true);
    ov.tool = t;
    OverlayRefreshChrome();
}

void ApplyCmd(Cmd c) {
    Overlay& ov = g_ov;
    switch (c) {
    case Cmd::ToolSelect:    SetTool(Tool::Select);    break;
    case Cmd::ToolPen:       SetTool(Tool::Pen);       break;
    case Cmd::ToolLine:      SetTool(Tool::Line);      break;
    case Cmd::ToolArrow:     SetTool(Tool::Arrow);     break;
    case Cmd::ToolRect:      SetTool(Tool::Rect);      break;
    case Cmd::ToolEllipse:   SetTool(Tool::Ellipse);   break;
    case Cmd::ToolHighlight: SetTool(Tool::Highlight); break;
    case Cmd::ToolText:      SetTool(Tool::Text);      break;
    case Cmd::ToolCounter:   SetTool(Tool::Counter);   break;
    case Cmd::ToolPixelate:  SetTool(Tool::Pixelate);  break;

    case Cmd::Color0: case Cmd::Color1: case Cmd::Color2:
    case Cmd::Color3: case Cmd::Color4: case Cmd::Color5:
        ov.color = kPalette[(int)c - (int)Cmd::Color0];
        if (ov.editing && ov.editIndex < ov.shapes.size())
            ov.shapes[ov.editIndex].color = ov.color;
        InvalidateRect(ov.hwnd, nullptr, FALSE);
        break;

    case Cmd::Width:
        ov.width = (ov.width >= 8) ? 1 : ov.width + (ov.width >= 4 ? 2 : 1);
        OverlayRefreshChrome();
        break;

    case Cmd::Undo:  Undo();  break;
    case Cmd::Redo:  Redo();  break;
    case Cmd::Copy:  if (DoCopy()) CloseOverlay(); break;
    case Cmd::Save:  DoSave(false); break;
    case Cmd::Close: CloseOverlay(); break;
    default: break;
    }
}

int StrokePx() { return max(1, (int)(g_ov.width * g_ov.scale + 0.5f)); }

void CommitLive() {
    Overlay& ov = g_ov;
    if (!ov.drawing) return;
    ov.drawing = false;

    bool worth = true;
    if (ov.live.pts.size() >= 2) {
        const POINT a = ov.live.pts.front(), b = ov.live.pts.back();
        if (ov.live.tool != Tool::Pen && abs(a.x - b.x) < 3 && abs(a.y - b.y) < 3)
            worth = false;
    }
    if (worth) {
        ov.shapes.push_back(ov.live);
        ov.undone.clear();
    }
    ov.live = Shape{};
    InvalidateRect(ov.hwnd, nullptr, FALSE);
}

} // namespace

// ---------------------------------------------------------------------------
//  Damage tracking.  Every interaction repaints only the pixels whose
//  appearance actually changed, which is what keeps the overlay smooth on
//  large or high-DPI desktops.
// ---------------------------------------------------------------------------
namespace {

RECT s_lastLive{};

void DamageFrame(const RECT& r, int t) {
    if (RectEmptyish(r)) return;
    OverlayDamage(RECT{ r.left - t,  r.top - t,    r.right + t, r.top + t    });
    OverlayDamage(RECT{ r.left - t,  r.bottom - t, r.right + t, r.bottom + t });
    OverlayDamage(RECT{ r.left - t,  r.top - t,    r.left + t,  r.bottom + t });
    OverlayDamage(RECT{ r.right - t, r.top - t,    r.right + t, r.bottom + t });
}

void DamageSelChange(const RECT& a, const RECT& b) {
    if (EqualRect(&a, &b)) return;
    HRGN ra = CreateRectRgn(a.left, a.top, a.right, a.bottom);
    HRGN rb = CreateRectRgn(b.left, b.top, b.right, b.bottom);
    CombineRgn(ra, ra, rb, RGN_XOR);
    if (g_ov.hwnd) InvalidateRgn(g_ov.hwnd, ra, FALSE);
    DeleteObject(ra);
    DeleteObject(rb);
    const int t = Sc(14);
    DamageFrame(a, t);
    DamageFrame(b, t);
}

void DamageGuides(POINT c) {
    const int t = Sc(1) + 1;
    OverlayDamage(RECT{ 0, c.y - 1, g_ov.cap.w, c.y + t });
    OverlayDamage(RECT{ c.x - 1, 0, c.x + t, g_ov.cap.h });
}

RECT ShapeBounds(const Shape& s) {
    RECT r{ 0, 0, 0, 0 };
    if (s.pts.empty()) return r;
    r = RECT{ s.pts[0].x, s.pts[0].y, s.pts[0].x, s.pts[0].y };
    for (const POINT& p : s.pts) {
        r.left   = min(r.left,   p.x);
        r.top    = min(r.top,    p.y);
        r.right  = max(r.right,  p.x);
        r.bottom = max(r.bottom, p.y);
    }
    int pad = s.width * 6 + 24;
    if (s.tool == Tool::Text)    pad = TextFontSize(s.width) * 3 + 24;
    if (s.tool == Tool::Counter) pad = s.width * 4 + 32;
    InflateRect(&r, pad, pad);
    return r;
}

void DamageLive() {
    const RECT now = g_ov.drawing ? ShapeBounds(g_ov.live) : RECT{ 0, 0, 0, 0 };
    OverlayDamage(s_lastLive);
    OverlayDamage(now);
    s_lastLive = now;
}

void BeginShape(POINT p) {
    Overlay& ov = g_ov;
    ov.live        = Shape{};
    ov.live.tool   = ov.tool;
    ov.live.color  = ov.color;
    ov.live.width  = StrokePx();
    ov.live.pts    = { p, p };
    ov.drawing     = true;
    DamageLive();
}

void PlaceText(POINT p) {
    Overlay& ov = g_ov;
    EndTextEdit(true);

    Shape s;
    s.tool  = Tool::Text;
    s.color = ov.color;
    s.width = StrokePx();
    s.pts   = { p };
    ov.shapes.push_back(s);
    ov.undone.clear();

    ov.editing   = true;
    ov.editIndex = ov.shapes.size() - 1;
    ov.caretOn   = true;
    SetTimer(ov.hwnd, kTimerCaret, 530, nullptr);
    InvalidateRect(ov.hwnd, nullptr, FALSE);
}

void PlaceCounter(POINT p) {
    Overlay& ov = g_ov;
    Shape s;
    s.tool   = Tool::Counter;
    s.color  = ov.color;
    s.width  = StrokePx();
    s.number = ov.counter++;
    s.pts    = { p };
    ov.shapes.push_back(s);
    ov.undone.clear();
    OverlayDamage(ShapeBounds(s));
}

void ResizeBy(Grip g, POINT delta, RECT& r) {
    switch (g) {
    case Grip::N:  r.top    += delta.y; break;
    case Grip::S:  r.bottom += delta.y; break;
    case Grip::W:  r.left   += delta.x; break;
    case Grip::E:  r.right  += delta.x; break;
    case Grip::NW: r.top += delta.y; r.left  += delta.x; break;
    case Grip::NE: r.top += delta.y; r.right += delta.x; break;
    case Grip::SW: r.bottom += delta.y; r.left  += delta.x; break;
    case Grip::SE: r.bottom += delta.y; r.right += delta.x; break;
    case Grip::Inside:
        OffsetRect(&r, delta.x, delta.y);
        break;
    default: break;
    }
    if (r.left > r.right)  std::swap(r.left, r.right);
    if (r.top  > r.bottom) std::swap(r.top,  r.bottom);
}

} // namespace

// ---------------------------------------------------------------------------
//  Input
// ---------------------------------------------------------------------------
namespace {

void OnMouseMove(POINT p) {
    Overlay&    ov     = g_ov;
    const POINT before = ov.cursor;
    const RECT  oldSel = ov.sel;

    ov.cursor = p;
    SyncMonitor();

    if (ov.mouseDown && ov.phase == Phase::Dragging) {
        ov.sel    = Normalized(ov.anchor, p);
        ov.hasSel = true;
        ClampSel();
        DamageSelChange(oldSel, ov.sel);
    } else if (ov.mouseDown && ov.drawing) {
        if (ov.live.tool == Tool::Pen) {
            const POINT last = ov.live.pts.back();
            if (abs(last.x - p.x) + abs(last.y - p.y) >= 2) ov.live.pts.push_back(p);
        } else {
            ov.live.pts[1] = p;
        }
        DamageLive();
    } else if (ov.mouseDown && ov.grip != Grip::None) {
        RECT r = ov.dragBase;
        ResizeBy(ov.grip, POINT{ p.x - ov.anchor.x, p.y - ov.anchor.y }, r);
        ov.sel = r;
        ClampSel();
        LayoutToolbar();
        DamageSelChange(oldSel, ov.sel);
    } else {
        const Cmd  wasCmd  = ov.hotCmd;
        const Grip wasGrip = ov.hotGrip;
        ov.hotCmd  = HitToolbar(p);
        ov.hotGrip = (ov.hotCmd == Cmd::None) ? HitGrip(p) : Grip::None;

        if (ov.phase == Phase::Idle) {
            const RECT wasSnap = ov.snap;
            const bool had     = ov.hasSnap;
            RECT hit;
            ov.hasSnap = ov.targets.hit(p, hit);
            if (ov.hasSnap) ov.snap = hit;
            if (had != ov.hasSnap || !EqualRect(&wasSnap, &ov.snap)) {
                if (had)        { OverlayDamage(wasSnap); DamageFrame(wasSnap, Sc(4)); }
                if (ov.hasSnap) { OverlayDamage(ov.snap); DamageFrame(ov.snap, Sc(4)); }
            }
            DamageGuides(before);
            DamageGuides(p);
        }
        if (wasCmd != ov.hotCmd || wasGrip != ov.hotGrip) OverlayRefreshChrome();
    }

    OverlayRefreshChrome();
    ov.moved = true;
}

void OnLButtonDown(POINT p) {
    Overlay& ov = g_ov;
    ov.cursor = p;
    SyncMonitor();

    const Cmd hit = HitToolbar(p);
    if (hit != Cmd::None) {
        ov.pressedCmd = hit;
        ov.hotCmd     = hit;
        OverlayRefreshChrome();
        return;
    }

    if (ov.editing) EndTextEdit(true);

    SetCapture(ov.hwnd);
    ov.mouseDown = true;
    ov.moved     = false;
    ov.anchor    = p;

    if (ov.phase == Phase::Ready) {
        const Grip g = HitGrip(p);
        const bool inside = PtInRect(&ov.sel, p) != 0;

        if (g != Grip::None && g != Grip::Inside) {           // resize edge
            ov.grip     = g;
            ov.dragBase = ov.sel;
            OverlayRefreshChrome();
            return;
        }
        if (inside && ov.tool != Tool::Select) {              // annotate
            if      (ov.tool == Tool::Text)    { PlaceText(p);    ov.mouseDown = false; ReleaseCapture(); }
            else if (ov.tool == Tool::Counter) { PlaceCounter(p); ov.mouseDown = false; ReleaseCapture(); }
            else                                 BeginShape(p);
            return;
        }
        if (inside) {                                         // move selection
            ov.grip     = Grip::Inside;
            ov.dragBase = ov.sel;
            return;
        }
        // Clicking outside the selection starts a fresh one.
        ov.shapes.clear();
        ov.undone.clear();
        ov.counter = 1;
    }

    const RECT oldSel = ov.sel;
    ov.phase  = Phase::Dragging;
    ov.grip   = Grip::None;
    ov.sel    = RECT{ p.x, p.y, p.x, p.y };
    ov.hasSel = true;
    LayoutToolbar();
    DamageSelChange(oldSel, ov.sel);
    OverlayRefreshChrome();
}

void OnLButtonUp(POINT p) {
    Overlay& ov = g_ov;

    if (ov.pressedCmd != Cmd::None) {
        const Cmd c = ov.pressedCmd;
        ov.pressedCmd = Cmd::None;
        if (HitToolbar(p) == c) ApplyCmd(c);
        return;
    }

    if (!ov.mouseDown) return;
    ov.mouseDown = false;
    ReleaseCapture();

    if (ov.drawing) { CommitLive(); return; }

    if (ov.phase == Phase::Dragging) {
        const bool tiny = RectW(ov.sel) < 4 || RectH(ov.sel) < 4;
        if (tiny) {
            RECT hit;
            if (ov.targets.hit(p, hit)) {          // click-to-grab a window
                const RECT oldSel = ov.sel;
                ov.sel = hit;
                ClampSel();
                DamageSelChange(oldSel, ov.sel);
            } else {
                ov.hasSel = false;
                ov.phase  = Phase::Idle;
                InvalidateRect(ov.hwnd, nullptr, FALSE);
                return;
            }
        }
        ov.phase   = Phase::Ready;
        ov.hasSnap = false;
    }

    ov.grip = Grip::None;
    LayoutToolbar();
    InvalidateRect(ov.hwnd, nullptr, FALSE);
}

} // namespace

namespace {

void ClearSelection() {
    Overlay&   ov     = g_ov;
    const RECT oldSel = ov.sel;
    ov.hasSel = false;
    ov.phase  = Phase::Idle;
    ov.grip   = Grip::None;
    ov.shapes.clear();
    ov.undone.clear();
    ov.counter = 1;
    SetRectEmpty(&ov.sel);
    LayoutToolbar();
    DamageSelChange(oldSel, ov.sel);
    InvalidateRect(ov.hwnd, nullptr, FALSE);
}

void NudgeSelection(int dx, int dy, bool resize) {
    Overlay&   ov     = g_ov;
    const RECT oldSel = ov.sel;
    if (resize) { ov.sel.right += dx; ov.sel.bottom += dy; }
    else        OffsetRect(&ov.sel, dx, dy);
    ClampSel();
    LayoutToolbar();
    DamageSelChange(oldSel, ov.sel);
    OverlayRefreshChrome();
}

void CopyHexUnderCursor() {
    const uint32_t rgb = g_ov.cap.at(g_ov.cursor.x, g_ov.cursor.y);
    wchar_t hex[16];
    wsprintfW(hex, L"#%02X%02X%02X",
              (int)((rgb >> 16) & 0xFF), (int)((rgb >> 8) & 0xFF), (int)(rgb & 0xFF));
    CopyTextToClipboard(g_ov.hwnd, hex);
    Toast(L"Colour copied", hex, false);
    CloseOverlay();
}

void OnKeyDown(WPARAM vk) {
    Overlay&   ov    = g_ov;
    const bool ctrl  = GetKeyState(VK_CONTROL) < 0;
    const bool shift = GetKeyState(VK_SHIFT)   < 0;

    if (ov.editing && !ctrl) {
        switch (vk) {
        case VK_ESCAPE: EndTextEdit(false); return;
        case VK_RETURN:
            if (shift && ov.editIndex < ov.shapes.size()) {
                ov.shapes[ov.editIndex].text += L'\n';
                InvalidateRect(ov.hwnd, nullptr, FALSE);
            } else {
                EndTextEdit(true);
            }
            return;
        case VK_BACK:
            if (ov.editIndex < ov.shapes.size() && !ov.shapes[ov.editIndex].text.empty()) {
                ov.shapes[ov.editIndex].text.pop_back();
                InvalidateRect(ov.hwnd, nullptr, FALSE);
            }
            return;
        default: return;
        }
    }

    if (ctrl) {
        switch (vk) {
        case 'C': if (DoCopy()) CloseOverlay();          return;
        case 'S': DoSave(shift);                          return;
        case 'Z': shift ? Redo() : Undo();                return;
        case 'Y': Redo();                                 return;
        case 'A': {
            RECT mon;
            if (MonitorRectAt(ov.cap, ov.cursor, mon)) {
                const RECT oldSel = ov.sel;
                ov.sel    = mon;
                ov.hasSel = true;
                ov.phase  = Phase::Ready;
                LayoutToolbar();
                DamageSelChange(oldSel, ov.sel);
                OverlayRefreshChrome();
            }
            return;
        }
        default: break;
        }
    }

    const int step = shift ? 10 : 1;

    switch (vk) {
    case VK_ESCAPE:
        if (ov.tool != Tool::Select) { SetTool(Tool::Select); return; }
        if (ov.hasSel)               { ClearSelection();      return; }
        CloseOverlay();
        return;

    case VK_RETURN:
        if (ov.hasSel && DoCopy()) CloseOverlay();
        return;

    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: {
        const int dx = (vk == VK_LEFT) ? -step : (vk == VK_RIGHT) ? step : 0;
        const int dy = (vk == VK_UP)   ? -step : (vk == VK_DOWN)  ? step : 0;
        if (ov.hasSel && ov.phase == Phase::Ready) NudgeSelection(dx, dy, ctrl);
        else SetCursorPos(ov.cursor.x + ov.cap.ox + dx, ov.cursor.y + ov.cap.oy + dy);
        return;
    }

    case 'V': SetTool(Tool::Select);    return;
    case 'P': SetTool(Tool::Pen);       return;
    case 'L': SetTool(Tool::Line);      return;
    case 'A': SetTool(Tool::Arrow);     return;
    case 'R': SetTool(Tool::Rect);      return;
    case 'E': SetTool(Tool::Ellipse);   return;
    case 'H': SetTool(Tool::Highlight); return;
    case 'T': SetTool(Tool::Text);      return;
    case 'N': SetTool(Tool::Counter);   return;
    case 'B': SetTool(Tool::Pixelate);  return;

    case 'C':
        if (!ov.hasSel) CopyHexUnderCursor();
        return;

    case 'M':
        ov.magnifier = !ov.magnifier;
        InvalidateRect(ov.hwnd, nullptr, FALSE);
        return;

    case '1': case '2': case '3': case '4': case '5': case '6':
        ApplyCmd((Cmd)((int)Cmd::Color0 + (int)(vk - '1')));
        return;

    default:
        return;
    }
}

} // namespace

// ---------------------------------------------------------------------------
//  Window
// ---------------------------------------------------------------------------
namespace {

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Overlay& ov = g_ov;

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintOverlay(hdc, ps.rcPaint);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            LPCWSTR id = IDC_CROSS;
            if      (ov.hotCmd != Cmd::None)      id = IDC_HAND;
            else if (ov.tool != Tool::Select &&
                     ov.phase == Phase::Ready &&
                     PtInRect(&ov.sel, ov.cursor)) id = IDC_CROSS;
            else if (ov.hotGrip != Grip::None)     id = CursorFor(ov.hotGrip);
            SetCursor(LoadCursorW(nullptr, id));
            return TRUE;
        }
        break;

    case WM_MOUSEMOVE:
        OnMouseMove(POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
        return 0;

    case WM_LBUTTONDOWN:
        OnLButtonDown(POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
        return 0;

    case WM_LBUTTONUP:
        OnLButtonUp(POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
        return 0;

    case WM_LBUTTONDBLCLK: {
        const POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (ov.phase == Phase::Ready && ov.tool == Tool::Select &&
            PtInRect(&ov.sel, p) && DoCopy())
            CloseOverlay();
        return 0;
    }

    case WM_RBUTTONDOWN:
        if (ov.drawing) { ov.drawing = false; ov.live = Shape{}; DamageLive(); }
        else if (ov.editing)          EndTextEdit(false);
        else if (ov.tool != Tool::Select) SetTool(Tool::Select);
        else if (ov.hasSel)           ClearSelection();
        else                          CloseOverlay();
        return 0;

    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1;
        ov.width = Clampi(ov.width + delta, 1, 12);
        if (ov.editing && ov.editIndex < ov.shapes.size()) {
            ov.shapes[ov.editIndex].width = StrokePx();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        OverlayRefreshChrome();
        return 0;
    }

    case WM_CHAR:
        if (ov.editing && ov.editIndex < ov.shapes.size()) {
            const wchar_t ch = (wchar_t)wp;
            if (ch >= 32 && ch != 127) {
                ov.shapes[ov.editIndex].text += ch;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;

    case WM_KEYDOWN:
        OnKeyDown(wp);
        return 0;

    case WM_TIMER:
        if (wp == kTimerCaret && ov.editing) {
            ov.caretOn = !ov.caretOn;
            if (ov.editIndex < ov.shapes.size())
                OverlayDamage(ShapeBounds(ov.shapes[ov.editIndex]));
        } else if (wp == kTimerOrphan) {
            KillTimer(hwnd, kTimerOrphan);
            // We never won the foreground, so WM_ACTIVATEAPP will never fire
            // to close us.  Give the window one last chance, then get out of
            // the way rather than sit on the screen holding the capture.
            if (GetForegroundWindow() != hwnd && !ForceForeground(hwnd))
                CloseOverlay();
        }
        return 0;

    case WM_ACTIVATEAPP:
        if (!wp && !s_suppressDeactivate) CloseOverlay();
        return 0;

    case WM_CAPTURECHANGED:
        ov.mouseDown = false;
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kTimerCaret);
        KillTimer(hwnd, kTimerOrphan);
        ov.hwnd = nullptr;
        ov.reset();
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

bool OverlayIsActive() { return g_ov.hwnd != nullptr; }

// An overlay that exists but is not the foreground window, and is not being
// held open by one of our own dialogs, is no longer reachable by the keyboard.
// Whatever is in front of it owns the input, so the user cannot dismiss it.
bool OverlayIsStale() {
    return g_ov.hwnd && !s_suppressDeactivate &&
           GetForegroundWindow() != g_ov.hwnd;
}

void OverlayAbandon() { CloseOverlay(); }

bool OverlayRegisterClass(HINSTANCE hInst) {
    PaintInit();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DBLCLKS | CS_OWNDC;
    wc.lpfnWndProc   = OverlayProc;
    wc.hInstance     = hInst;
    wc.hCursor       = nullptr;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = WNDCLS_OVERLAY;
    return RegisterClassExW(&wc) != 0;
}

void OverlayShutdown() {
    if (g_ov.hwnd) DestroyWindow(g_ov.hwnd);
    g_ov.reset();
    PaintShutdown();
}

void OverlayShow(HINSTANCE hInst, Grab mode) {
    Overlay& ov = g_ov;
    if (ov.hwnd) { ForceForeground(ov.hwnd); return; }

    HWND target = (mode == Grab::ActiveWindow) ? GetForegroundWindow() : nullptr;

    const Tool     keepTool  = ov.tool;
    const COLORREF keepColor = ov.color;
    const int      keepWidth = ov.width;
    const bool     keepMag   = ov.magnifier;

    ov.reset();
    ov.tool      = keepTool;
    ov.color     = keepColor;
    ov.width     = keepWidth;
    ov.magnifier = keepMag;
    ov.hInst     = hInst;

    const long long tStart = TraceNow();

    if (!CaptureScreen(ov.cap)) {
        Toast(APP_NAME, L"Screen capture failed.", true);
        return;
    }
    const long long tCapture = TraceNow();

    CollectWindowTargets(ov.cap, ov.targets);
    const long long tTargets = TraceNow();

    POINT scr{};
    GetCursorPos(&scr);
    ov.cursor = POINT{ scr.x - ov.cap.ox, scr.y - ov.cap.oy };
    ov.cursor.x = Clampi(ov.cursor.x, 0, ov.cap.w - 1);
    ov.cursor.y = Clampi(ov.cursor.y, 0, ov.cap.h - 1);
    SyncMonitor();

    switch (mode) {
    case Grab::FullScreen: {
        RECT mon;
        if (MonitorRectAt(ov.cap, ov.cursor, mon)) {
            ov.sel = mon; ov.hasSel = true; ov.phase = Phase::Ready;
        }
        break;
    }
    case Grab::ActiveWindow: {
        RECT r;
        if (WindowRectOf(ov.cap, target, r)) {
            ov.sel = r; ov.hasSel = true; ov.phase = Phase::Ready;
        }
        break;
    }
    case Grab::Repeat:
        if (s_haveLastSel) {
            ov.sel = s_lastSel; ov.hasSel = true; ov.phase = Phase::Ready;
            ClampSel();
        }
        break;
    case Grab::Region:
    default:
        break;
    }

    ov.hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOPARENTNOTIFY,
        WNDCLS_OVERLAY, APP_NAME, WS_POPUP,
        ov.cap.ox, ov.cap.oy, ov.cap.w, ov.cap.h,
        nullptr, nullptr, hInst, nullptr);

    if (!ov.hwnd) { ov.reset(); return; }
    const long long tWindow = TraceNow();

    LayoutToolbar();
    ov.lastChrome = ChromeBounds();

    ShowWindow(ov.hwnd, SW_SHOW);
    const long long tShow = TraceNow();

    // If we lose the foreground race - an elevated window, or a game holding
    // the display exclusively - no WM_ACTIVATEAPP will ever arrive to close
    // us, and a silently orphaned overlay would swallow every later hotkey.
    // Arm a watchdog so the window cannot outlive its usefulness.
    const bool gotForeground = ForceForeground(ov.hwnd);
    if (!gotForeground) SetTimer(ov.hwnd, kTimerOrphan, kOrphanCheckMs, nullptr);
    const long long tForeground = TraceNow();

    UpdateWindow(ov.hwnd);              // forces the first WM_PAINT to complete
    const long long tPainted = TraceNow();

    if (g_trace) {
        wchar_t buf[320];
        wsprintfW(buf,
            L"capture %d.%02d  targets %d.%02d  createwnd %d.%02d  show %d.%02d  "
            L"foreground %d.%02d  firstpaint %d.%02d  TOTAL %d.%02d ms  (%dx%d)"
            L"%s",
            (int)TraceMs(tStart, tCapture),         (int)(TraceMs(tStart, tCapture) * 100) % 100,
            (int)TraceMs(tCapture, tTargets),       (int)(TraceMs(tCapture, tTargets) * 100) % 100,
            (int)TraceMs(tTargets, tWindow),        (int)(TraceMs(tTargets, tWindow) * 100) % 100,
            (int)TraceMs(tWindow, tShow),           (int)(TraceMs(tWindow, tShow) * 100) % 100,
            (int)TraceMs(tShow, tForeground),       (int)(TraceMs(tShow, tForeground) * 100) % 100,
            (int)TraceMs(tForeground, tPainted),    (int)(TraceMs(tForeground, tPainted) * 100) % 100,
            (int)TraceMs(tStart, tPainted),         (int)(TraceMs(tStart, tPainted) * 100) % 100,
            ov.cap.w, ov.cap.h,
            gotForeground ? L"" : L"  *** NO FOREGROUND ***");
        TraceLine(buf);
    }
}
