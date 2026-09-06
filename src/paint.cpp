// ---------------------------------------------------------------------------
//  All overlay rendering: dimming, annotations, selection chrome, toolbar,
//  magnifier, and the offscreen render used for export.
// ---------------------------------------------------------------------------
#include "overlay.h"

#include <algorithm>
using std::min;
using std::max;

#pragma warning(push, 3)
#include <objidl.h>
#include <gdiplus.h>
#pragma warning(pop)

using namespace Gdiplus;

// ---------------------------------------------------------------------------
//  Palette
// ---------------------------------------------------------------------------
namespace {

constexpr COLORREF kPanel     = RGB( 26,  27,  32);
constexpr COLORREF kPanelEdge = RGB( 68,  70,  80);
constexpr COLORREF kInk       = RGB(233, 234, 240);
constexpr COLORREF kInkMuted  = RGB(146, 149, 160);
constexpr COLORREF kHover     = RGB( 55,  57,  66);
constexpr COLORREF kDim       = RGB(  8,   9,  14);

const COLORREF kSwatches[6] = {
    RGB(255,  61,  61),   // red
    RGB(255, 173,  51),   // amber
    RGB( 60, 210, 120),   // green
    RGB( 76, 141, 255),   // blue
    RGB( 24,  25,  30),   // near-black
    RGB(255, 255, 255),   // white
};

ULONG_PTR   g_gdip   = 0;
HDC         g_dotDC  = nullptr;   // 1x1 surface used for constant-alpha fills
HBITMAP     g_dotBmp = nullptr;
uint32_t*   g_dotPx  = nullptr;

inline Color Argb(int a, COLORREF c) {
    return Color((BYTE)a, GetRValue(c), GetGValue(c), GetBValue(c));
}
inline Color Opaque(COLORREF c) { return Argb(255, c); }

// Constant-alpha rectangle fill.  Cheaper and more predictable than creating
// a brush per call, and it is the only blend the dimming path needs.
void FillAlpha(HDC dc, const RECT& r, COLORREF c, int alpha) {
    if (RectEmptyish(r) || alpha <= 0) return;
    if (!g_dotPx) return;
    *g_dotPx = 0xFF000000u | (uint32_t)(GetRValue(c) << 16) |
               (uint32_t)(GetGValue(c) << 8) | (uint32_t)GetBValue(c);
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, (BYTE)Clampi(alpha, 0, 255), 0 };
    AlphaBlend(dc, r.left, r.top, RectW(r), RectH(r), g_dotDC, 0, 0, 1, 1, bf);
}

void FillSolid(HDC dc, const RECT& r, COLORREF c) {
    if (RectEmptyish(r)) return;
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

// 1px frame drawn just outside `r`.
void FrameLines(HDC dc, const RECT& r, COLORREF c, int t) {
    RECT top{ r.left - t, r.top - t, r.right + t, r.top };
    RECT bot{ r.left - t, r.bottom,  r.right + t, r.bottom + t };
    RECT lef{ r.left - t, r.top,     r.left,      r.bottom };
    RECT rig{ r.right,    r.top,     r.right + t, r.bottom };
    FillSolid(dc, top, c); FillSolid(dc, bot, c);
    FillSolid(dc, lef, c); FillSolid(dc, rig, c);
}

GraphicsPath* RoundRectPath(const RectF& r, REAL rad) {
    GraphicsPath* p = new GraphicsPath();
    const REAL d = rad * 2;
    p->AddArc(r.X,             r.Y,             d, d, 180, 90);
    p->AddArc(r.GetRight() - d, r.Y,            d, d, 270, 90);
    p->AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p->AddArc(r.X,             r.GetBottom() - d, d, d, 90, 90);
    p->CloseFigure();
    return p;
}

void FillRoundRect(Graphics& g, const RectF& r, REAL rad, const Color& fill,
                   const Color* edge = nullptr, REAL edgeW = 1.0f) {
    GraphicsPath* p = RoundRectPath(r, rad);
    SolidBrush b(fill);
    g.FillPath(&b, p);
    if (edge) {
        // Stroke down the middle of the pixel so a 1px border lands on exactly
        // one row instead of being anti-aliased across two.
        GraphicsPath* q = RoundRectPath(
            RectF(r.X + 0.5f, r.Y + 0.5f, r.Width - 1.0f, r.Height - 1.0f), rad);
        Pen pen(*edge, edgeW);
        g.DrawPath(&pen, q);
        delete q;
    }
    delete p;
}

// ---------------------------------------------------------------------------
//  Text.
//
//  GDI+ DrawString lays glyphs out in float space with grayscale coverage,
//  which is why it reads soft next to native Windows UI.  GDI with ClearType
//  hints stems onto the pixel grid and uses subpixel coverage, so all chrome
//  goes through here instead.  Image content -- the text tool and the step
//  numbers -- deliberately uses grayscale, because subpixel fringes are wrong
//  in a file that will be scaled or viewed on a different display.
// ---------------------------------------------------------------------------
struct FontEntry { int px; int weight; BYTE quality; HFONT font; };
std::vector<FontEntry> g_fonts;
HDC g_measureDC = nullptr;

HFONT UiFont(int px, int weight, bool subpixel) {
    const BYTE q = subpixel ? CLEARTYPE_QUALITY : ANTIALIASED_QUALITY;
    for (const FontEntry& e : g_fonts)
        if (e.px == px && e.weight == weight && e.quality == q) return e.font;

    LOGFONTW lf{};
    lf.lfHeight         = -px;                 // em size in pixels
    lf.lfWeight         = weight;
    lf.lfCharSet        = DEFAULT_CHARSET;
    lf.lfQuality        = q;
    lf.lfOutPrecision   = OUT_TT_PRECIS;
    lf.lfClipPrecision  = CLIP_DEFAULT_PRECIS;
    lf.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
    lstrcpynW(lf.lfFaceName, L"Segoe UI", LF_FACESIZE);

    HFONT f = CreateFontIndirectW(&lf);
    g_fonts.push_back({ px, weight, q, f });
    return f;
}

SIZE MeasureText(const std::wstring& s, int px, int weight = FW_NORMAL,
                 bool subpixel = true) {
    SIZE out{ 0, px };
    if (!g_measureDC) return out;
    HGDIOBJ old = SelectObject(g_measureDC, UiFont(px, weight, subpixel));
    RECT r{ 0, 0, 0, 0 };
    DrawTextW(g_measureDC, s.c_str(), (int)s.size(), &r,
              DT_CALCRECT | DT_NOPREFIX | DT_NOCLIP);
    SelectObject(g_measureDC, old);
    out.cx = r.right - r.left;
    out.cy = r.bottom - r.top;
    return out;
}

void DrawTextCrisp(HDC dc, const std::wstring& s, RECT box, COLORREF color,
                   UINT flags, int px, int weight = FW_NORMAL,
                   bool subpixel = true) {
    HGDIOBJ        old      = SelectObject(dc, UiFont(px, weight, subpixel));
    const int      oldBk    = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(dc, color);
    DrawTextW(dc, s.c_str(), (int)s.size(), &box, flags | DT_NOPREFIX);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldBk);
    SelectObject(dc, old);
}

// Same, but onto a surface currently owned by a GDI+ Graphics.
void DrawTextCrisp(Graphics& g, const std::wstring& s, const RECT& box,
                   COLORREF color, UINT flags, int px, int weight = FW_NORMAL,
                   bool subpixel = true) {
    HDC dc = g.GetHDC();
    if (!dc) return;
    DrawTextCrisp(dc, s, box, color, flags, px, weight, subpixel);
    g.ReleaseHDC(dc);
}

// Flattens an alpha ink colour against the opaque panel behind it, since GDI
// text has no alpha of its own.
COLORREF Flatten(int alpha, COLORREF ink, COLORREF behind) {
    const int a = Clampi(alpha, 0, 255);
    auto mix = [&](int f, int b) { return (f * a + b * (255 - a)) / 255; };
    return RGB(mix(GetRValue(ink), GetRValue(behind)),
               mix(GetGValue(ink), GetGValue(behind)),
               mix(GetBValue(ink), GetBValue(behind)));
}

} // namespace

// ---------------------------------------------------------------------------
void PaintInit() {
    if (g_gdip) return;
    GdiplusStartupInput in;
    GdiplusStartup(&g_gdip, &in, nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = 1;
    bi.bmiHeader.biHeight      = -1;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    g_dotBmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    g_dotPx  = static_cast<uint32_t*>(bits);
    g_dotDC  = CreateCompatibleDC(nullptr);
    if (g_dotDC && g_dotBmp) SelectObject(g_dotDC, g_dotBmp);

    g_measureDC = CreateCompatibleDC(nullptr);
}

void PaintShutdown() {
    if (g_dotDC)  { DeleteDC(g_dotDC);      g_dotDC  = nullptr; }
    if (g_dotBmp) { DeleteObject(g_dotBmp); g_dotBmp = nullptr; }
    g_dotPx = nullptr;
    if (g_measureDC) { DeleteDC(g_measureDC); g_measureDC = nullptr; }
    for (const FontEntry& e : g_fonts) if (e.font) DeleteObject(e.font);
    g_fonts.clear();
    if (g_gdip)   { GdiplusShutdown(g_gdip); g_gdip = 0; }
}

int TextFontSize(int strokeWidth) { return 11 + strokeWidth * 4; }

// ---------------------------------------------------------------------------
//  Annotations
// ---------------------------------------------------------------------------
namespace {

PointF Dev(const POINT& p, int dx, int dy) {
    return PointF((REAL)(p.x - dx), (REAL)(p.y - dy));
}

void MosaicRect(Graphics& g, const Capture& base, RECT r, int dx, int dy, int block) {
    if (RectEmptyish(r)) return;
    const int w = RectW(r), h = RectH(r);
    const int sw = max(1, w / max(2, block));
    const int sh = max(1, h / max(2, block));

    HDC dc = g.GetHDC();
    if (dc) {
        HDC     tmp = CreateCompatibleDC(base.dc);
        HBITMAP tb  = CreateCompatibleBitmap(base.dc, sw, sh);
        if (tmp && tb) {
            HGDIOBJ old = SelectObject(tmp, tb);
            SetStretchBltMode(tmp, HALFTONE);        // average down
            SetBrushOrgEx(tmp, 0, 0, nullptr);
            StretchBlt(tmp, 0, 0, sw, sh, base.dc, r.left, r.top, w, h, SRCCOPY);
            SetStretchBltMode(dc, COLORONCOLOR);     // hard blocks back up
            StretchBlt(dc, r.left - dx, r.top - dy, w, h, tmp, 0, 0, sw, sh, SRCCOPY);
            SelectObject(tmp, old);
        }
        if (tb)  DeleteObject(tb);
        if (tmp) DeleteDC(tmp);
        g.ReleaseHDC(dc);
    }
}

void DrawArrowHead(Graphics& g, PointF a, PointF b, REAL w, const Color& c) {
    const REAL vx = b.X - a.X, vy = b.Y - a.Y;
    const REAL len = sqrtf(vx * vx + vy * vy);
    if (len < 0.5f) return;
    const REAL ux = vx / len, uy = vy / len;
    const REAL hl = 9.0f + w * 3.0f;
    const REAL hh = 4.5f + w * 1.7f;
    const PointF base(b.X - ux * hl, b.Y - uy * hl);
    const PointF head[3] = {
        b,
        PointF(base.X - uy * hh, base.Y + ux * hh),
        PointF(base.X + uy * hh, base.Y - ux * hh)
    };
    SolidBrush br(c);
    g.FillPolygon(&br, head, 3);
}

void DrawShape(Graphics& g, const Capture& base, const Shape& s, int dx, int dy) {
    if (s.pts.empty()) return;

    const REAL      w  = (REAL)max(1, s.width);
    const PointF    p0 = Dev(s.pts[0], dx, dy);
    const PointF    p1 = Dev(s.pts[s.pts.size() - 1], dx, dy);
    const RectF     box((REAL)min(p0.X, p1.X), (REAL)min(p0.Y, p1.Y),
                        fabsf(p1.X - p0.X), fabsf(p1.Y - p0.Y));

    switch (s.tool) {
    case Tool::Pen: {
        Pen pen(Opaque(s.color), w);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        pen.SetLineJoin(LineJoinRound);
        if (s.pts.size() == 1) {
            SolidBrush br(Opaque(s.color));
            g.FillEllipse(&br, p0.X - w / 2, p0.Y - w / 2, w, w);
        } else {
            std::vector<PointF> pts;
            pts.reserve(s.pts.size());
            for (const POINT& p : s.pts) pts.push_back(Dev(p, dx, dy));
            if (pts.size() > 2) g.DrawCurve(&pen, pts.data(), (INT)pts.size(), 0.28f);
            else                g.DrawLines(&pen, pts.data(), (INT)pts.size());
        }
        break;
    }
    case Tool::Line: {
        Pen pen(Opaque(s.color), w);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        g.DrawLine(&pen, p0, p1);
        break;
    }
    case Tool::Arrow: {
        const REAL vx = p1.X - p0.X, vy = p1.Y - p0.Y;
        const REAL len = sqrtf(vx * vx + vy * vy);
        if (len < 1.0f) break;
        const REAL hl = 9.0f + w * 3.0f;
        const REAL back = max(0.0f, len - hl * 0.82f) / len;
        Pen pen(Opaque(s.color), w);
        pen.SetStartCap(LineCapRound);
        g.DrawLine(&pen, p0, PointF(p0.X + vx * back, p0.Y + vy * back));
        DrawArrowHead(g, p0, p1, w, Opaque(s.color));
        break;
    }
    case Tool::Rect: {
        Pen pen(Opaque(s.color), w);
        pen.SetLineJoin(LineJoinRound);
        if (box.Width >= 1 && box.Height >= 1) g.DrawRectangle(&pen, box);
        break;
    }
    case Tool::Ellipse: {
        Pen pen(Opaque(s.color), w);
        if (box.Width >= 1 && box.Height >= 1) g.DrawEllipse(&pen, box);
        break;
    }
    case Tool::Highlight: {
        Pen pen(Argb(96, s.color), w * 5.0f + 6.0f);
        pen.SetStartCap(LineCapFlat);
        pen.SetEndCap(LineCapFlat);
        g.DrawLine(&pen, p0, p1);
        break;
    }
    case Tool::Pixelate: {
        RECT r = Normalized(s.pts[0], s.pts[s.pts.size() - 1]);
        MosaicRect(g, base, r, dx, dy, max(5, s.width * 3));
        break;
    }
    case Tool::Counter: {
        const REAL rad = 9.0f + w * 2.6f;
        SolidBrush fill(Opaque(s.color));
        g.FillEllipse(&fill, p0.X - rad, p0.Y - rad, rad * 2, rad * 2);
        Pen ring(Argb(70, RGB(0, 0, 0)), 1.0f);
        g.DrawEllipse(&ring, p0.X - rad, p0.Y - rad, rad * 2, rad * 2);

        wchar_t num[12];
        wsprintfW(num, L"%d", s.number);
        const RECT numBox{ (int)(p0.X - rad), (int)(p0.Y - rad),
                           (int)(p0.X + rad), (int)(p0.Y + rad) };
        DrawTextCrisp(g, num, numBox, RGB(255, 255, 255),
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE,
                      max(8, (int)(rad * 1.15f)), FW_SEMIBOLD, false);
        break;
    }
    case Tool::Text: {
        if (s.text.empty()) break;
        const int  px = TextFontSize(s.width);
        const SIZE m  = MeasureAnnotationText(s.text, s.width);
        const RECT textBox{ (int)p0.X, (int)p0.Y,
                            (int)p0.X + m.cx + px, (int)p0.Y + m.cy + px };
        DrawTextCrisp(g, s.text, textBox, s.color,
                      DT_LEFT | DT_TOP | DT_NOCLIP, px, FW_NORMAL, false);
        break;
    }
    default:
        break;
    }
}

void DrawShapes(HDC dc, const Capture& base, const std::vector<Shape>& list,
                const Shape* live, int dx, int dy, const RECT& clip) {
    if (list.empty() && !live) return;

    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetClip(Rect(clip.left - dx, clip.top - dy, RectW(clip), RectH(clip)));

    for (const Shape& s : list) DrawShape(g, base, s, dx, dy);
    if (live) DrawShape(g, base, *live, dx, dy);
}

} // namespace

SIZE MeasureAnnotationText(const std::wstring& text, int strokeWidth) {
    // Must match how Tool::Text draws, or the caret drifts off the last glyph.
    const int px = TextFontSize(strokeWidth);
    return MeasureText(text.empty() ? L" " : text, px, FW_NORMAL, false);
}

bool RenderExport(const RECT& region, Capture& out) {
    if (!CaptureRegionBitmap(g_ov.cap, region, out)) return false;

    RECT full{ region.left, region.top, region.left + out.w, region.top + out.h };
    DrawShapes(out.dc, g_ov.cap, g_ov.shapes, nullptr, region.left, region.top, full);

    uint32_t* p   = out.px;
    uint32_t* end = p + (size_t)out.w * out.h;
    for (; p != end; ++p) *p |= 0xFF000000u;       // GDI/GDI+ leave alpha unset
    return true;
}

// ---------------------------------------------------------------------------
//  Toolbar
// ---------------------------------------------------------------------------
namespace {

struct ItemDef { Cmd cmd; const wchar_t* tip; const wchar_t* key; };

const ItemDef kItems[] = {
    { Cmd::ToolSelect,    L"Move / resize",      L"V" },
    { Cmd::ToolPen,       L"Pen",                L"P" },
    { Cmd::ToolLine,      L"Line",               L"L" },
    { Cmd::ToolArrow,     L"Arrow",              L"A" },
    { Cmd::ToolRect,      L"Rectangle",          L"R" },
    { Cmd::ToolEllipse,   L"Ellipse",            L"E" },
    { Cmd::ToolHighlight, L"Highlighter",        L"H" },
    { Cmd::ToolText,      L"Text",               L"T" },
    { Cmd::ToolCounter,   L"Numbered step",      L"N" },
    { Cmd::ToolPixelate,  L"Pixelate",           L"B" },
    { Cmd::Sep1,          nullptr,               nullptr },
    { Cmd::Color0,        L"Red",                L"1" },
    { Cmd::Color1,        L"Amber",              L"2" },
    { Cmd::Color2,        L"Green",              L"3" },
    { Cmd::Color3,        L"Blue",               L"4" },
    { Cmd::Color4,        L"Black",              L"5" },
    { Cmd::Color5,        L"White",              L"6" },
    { Cmd::Width,         L"Thickness",          L"Wheel" },
    { Cmd::Sep2,          nullptr,               nullptr },
    { Cmd::Undo,          L"Undo",               L"Ctrl+Z" },
    { Cmd::Redo,          L"Redo",               L"Ctrl+Y" },
    { Cmd::Sep3,          nullptr,               nullptr },
    { Cmd::Copy,          L"Copy to clipboard",  L"Enter" },
    { Cmd::Save,          L"Save as\u2026",      L"Ctrl+S" },
    { Cmd::Close,         L"Cancel",             L"Esc" },
};

inline bool IsSep(Cmd c)     { return c == Cmd::Sep1 || c == Cmd::Sep2 || c == Cmd::Sep3; }
inline bool IsSwatch(Cmd c)  { return c >= Cmd::Color0 && c <= Cmd::Color5; }
inline int  SwatchIndex(Cmd c) { return (int)c - (int)Cmd::Color0; }

inline Tool CmdToTool(Cmd c) {
    switch (c) {
    case Cmd::ToolSelect:    return Tool::Select;
    case Cmd::ToolPen:       return Tool::Pen;
    case Cmd::ToolLine:      return Tool::Line;
    case Cmd::ToolArrow:     return Tool::Arrow;
    case Cmd::ToolRect:      return Tool::Rect;
    case Cmd::ToolEllipse:   return Tool::Ellipse;
    case Cmd::ToolHighlight: return Tool::Highlight;
    case Cmd::ToolText:      return Tool::Text;
    case Cmd::ToolCounter:   return Tool::Counter;
    case Cmd::ToolPixelate:  return Tool::Pixelate;
    default:                 return Tool::COUNT;
    }
}

int S(int v) { return max(1, (int)(v * g_ov.scale + 0.5f)); }

// Chrome attached to the selection must be clamped to the monitor the
// selection sits on, not the one the cursor happens to be over.
RECT MonitorFor(POINT p) {
    RECT m;
    if (MonitorRectAt(g_ov.cap, p, m)) return m;
    return RECT{ 0, 0, g_ov.cap.w, g_ov.cap.h };
}

} // namespace

void LayoutToolbar() {
    Overlay& ov = g_ov;
    ov.buttons.clear();
    ov.toolbarVisible = false;
    SetRectEmpty(&ov.toolbar);

    if (!ov.hasSel || ov.phase != Phase::Ready) return;

    const int btn   = S(28);
    const int gap   = S(2);
    const int sw    = S(22);
    const int sep   = S(11);
    const int pad   = S(6);
    const int h     = btn + pad * 2;

    int wTotal = pad * 2;
    for (const ItemDef& d : kItems)
        wTotal += IsSep(d.cmd) ? sep : (IsSwatch(d.cmd) ? sw : btn + gap);

    const RECT mon = MonitorFor(POINT{ ov.sel.right - 1, ov.sel.bottom - 1 });

    const int margin = S(10);
    int x = ov.sel.right - wTotal;
    int y = ov.sel.bottom + margin;

    if (y + h > mon.bottom - S(4)) {                 // no room below
        const int above = ov.sel.top - margin - h;
        y = (above >= mon.top + S(4)) ? above
                                      : max(mon.top + S(4), ov.sel.bottom - h - margin);
    }
    x = Clampi(x, mon.left + S(4), max(mon.left + S(4), mon.right - wTotal - S(4)));
    y = Clampi(y, mon.top  + S(4), max(mon.top  + S(4), mon.bottom - h - S(4)));

    ov.toolbar = RECT{ x, y, x + wTotal, y + h };

    int cx = x + pad;
    for (const ItemDef& d : kItems) {
        ToolButton b{ d.cmd, d.tip, d.key, RECT{} };
        if (IsSep(d.cmd)) {
            b.rc = RECT{ cx, y + pad, cx + sep, y + pad + btn };
            cx += sep;
        } else if (IsSwatch(d.cmd)) {
            b.rc = RECT{ cx, y + pad, cx + sw, y + pad + btn };
            cx += sw;
        } else {
            b.rc = RECT{ cx, y + pad, cx + btn, y + pad + btn };
            cx += btn + gap;
        }
        ov.buttons.push_back(b);
    }
    ov.toolbarVisible = true;
}

Cmd HitToolbar(POINT p) {
    Overlay& ov = g_ov;
    if (!ov.toolbarVisible) return Cmd::None;
    if (!PtInRect(&ov.toolbar, p))  return Cmd::None;
    for (const ToolButton& b : ov.buttons) {
        if (IsSep(b.cmd)) continue;
        if (PtInRect(&b.rc, p)) return b.cmd;
    }
    return Cmd::None;
}

// ---------------------------------------------------------------------------
//  Toolbar icons.  Everything is drawn in a normalised 0..1 box so the same
//  geometry works at any DPI.
// ---------------------------------------------------------------------------
namespace {

inline PointF N(const RectF& r, REAL x, REAL y) {
    return PointF(r.X + r.Width * x, r.Y + r.Height * y);
}
inline RectF NR(const RectF& r, REAL x, REAL y, REAL w, REAL h) {
    return RectF(r.X + r.Width * x, r.Y + r.Height * y, r.Width * w, r.Height * h);
}

// Letter-shaped icons (the "T" and the step-counter "1").  GDI has no alpha,
// so a dimmed ink colour is flattened against the panel it sits on.
void Glyph(Graphics& g, const RectF& r, const wchar_t* ch, const Color& ink, REAL sizeFrac) {
    const COLORREF solid = Flatten(ink.GetA(), RGB(ink.GetR(), ink.GetG(), ink.GetB()), kPanel);
    const RECT box{ (int)r.X, (int)r.Y, (int)r.GetRight(), (int)r.GetBottom() };
    DrawTextCrisp(g, ch, box, solid, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP,
                  max(8, (int)(r.Height * sizeFrac)), FW_SEMIBOLD, false);
}

void DrawCmdIcon(Graphics& g, Cmd c, const RectF& r, const Color& ink) {
    const REAL t = max(1.3f, r.Width * 0.115f);
    Pen pen(ink, t);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    SolidBrush brush(ink);

    switch (c) {
    case Cmd::ToolSelect: {
        const PointF p[7] = {
            N(r,0.30f,0.08f), N(r,0.30f,0.86f), N(r,0.46f,0.68f),
            N(r,0.58f,0.94f), N(r,0.71f,0.87f), N(r,0.59f,0.62f), N(r,0.80f,0.58f)
        };
        g.FillPolygon(&brush, p, 7);
        break;
    }
    case Cmd::ToolPen: {
        // A filled pencil body, so it never reads as a plain diagonal line.
        const PointF body[5] = {
            N(r,0.10f,0.90f), N(r,0.22f,0.60f), N(r,0.70f,0.10f),
            N(r,0.90f,0.30f), N(r,0.40f,0.78f)
        };
        g.FillPolygon(&brush, body, 5);
        Pen cut(Argb(ink.GetA() / 3, kPanel), max(1.0f, t * 0.7f));
        g.DrawLine(&cut, N(r,0.22f,0.60f), N(r,0.40f,0.78f));
        break;
    }
    case Cmd::ToolLine:
        g.DrawLine(&pen, N(r,0.14f,0.86f), N(r,0.86f,0.14f));
        break;
    case Cmd::ToolArrow: {
        g.DrawLine(&pen, N(r,0.14f,0.86f), N(r,0.68f,0.32f));
        const PointF h[3] = { N(r,0.90f,0.10f), N(r,0.52f,0.18f), N(r,0.82f,0.48f) };
        g.FillPolygon(&brush, h, 3);
        break;
    }
    case Cmd::ToolRect:
        g.DrawRectangle(&pen, NR(r, 0.13f, 0.22f, 0.74f, 0.56f));
        break;
    case Cmd::ToolEllipse:
        g.DrawEllipse(&pen, NR(r, 0.10f, 0.18f, 0.80f, 0.64f));
        break;
    case Cmd::ToolHighlight: {
        Pen wide(Color((BYTE)(ink.GetA() / 3), ink.GetR(), ink.GetG(), ink.GetB()),
                 r.Width * 0.30f);
        wide.SetStartCap(LineCapFlat);
        wide.SetEndCap(LineCapFlat);
        g.DrawLine(&wide, N(r,0.10f,0.58f), N(r,0.90f,0.22f));
        g.DrawLine(&pen,  N(r,0.10f,0.92f), N(r,0.90f,0.92f));
        break;
    }
    case Cmd::ToolText:
        Glyph(g, r, L"T", ink, 0.92f);
        break;
    case Cmd::ToolCounter:
        g.DrawEllipse(&pen, NR(r, 0.08f, 0.08f, 0.84f, 0.84f));
        Glyph(g, NR(r, 0.08f, 0.08f, 0.84f, 0.84f), L"1", ink, 0.62f);
        break;
    case Cmd::ToolPixelate: {
        // A mosaic of varying density reads as "pixelate"; a checkerboard
        // just reads as a diamond at this size.
        static const int shade[9] = { 100, 34, 74, 40, 90, 28, 78, 44, 100 };
        const REAL cell = r.Width * 0.26f;
        for (int i = 0; i < 9; ++i) {
            SolidBrush b(Color((BYTE)(ink.GetA() * shade[i] / 100),
                               ink.GetR(), ink.GetG(), ink.GetB()));
            g.FillRectangle(&b, RectF(r.X + r.Width  * 0.11f + cell * (i % 3),
                                      r.Y + r.Height * 0.11f + cell * (i / 3),
                                      cell * 0.86f, cell * 0.86f));
        }
        break;
    }
    case Cmd::Undo:
    case Cmd::Redo: {
        // A half-circle "rainbow" with a head dropped on one end.  Placing the
        // head at a real arc endpoint keeps the two icons readable as a pair.
        const bool  back = (c == Cmd::Undo);
        const RectF box  = NR(r, 0.12f, 0.30f, 0.76f, 0.68f);
        g.DrawArc(&pen, box, 180.0f, 180.0f);

        const REAL ex = back ? box.X : box.GetRight();
        const REAL ey = box.Y + box.Height * 0.5f;
        const REAL hw = r.Width * 0.15f, hh = r.Height * 0.24f;
        const PointF h[3] = {
            PointF(ex, ey + hh), PointF(ex - hw, ey - hh * 0.15f),
            PointF(ex + hw, ey - hh * 0.15f)
        };
        g.FillPolygon(&brush, h, 3);
        break;
    }
    case Cmd::Copy: {
        g.DrawRectangle(&pen, NR(r, 0.08f, 0.08f, 0.54f, 0.54f));
        g.FillRectangle(&brush, NR(r, 0.38f, 0.38f, 0.54f, 0.54f));
        break;
    }
    case Cmd::Save: {
        g.DrawLine(&pen, N(r,0.50f,0.08f), N(r,0.50f,0.52f));
        const PointF h[3] = { N(r,0.50f,0.74f), N(r,0.24f,0.42f), N(r,0.76f,0.42f) };
        g.FillPolygon(&brush, h, 3);
        g.DrawLine(&pen, N(r,0.14f,0.92f), N(r,0.86f,0.92f));
        break;
    }
    case Cmd::Close:
        g.DrawLine(&pen, N(r,0.20f,0.20f), N(r,0.80f,0.80f));
        g.DrawLine(&pen, N(r,0.80f,0.20f), N(r,0.20f,0.80f));
        break;
    default:
        break;
    }
}

} // namespace

namespace {

void DrawTooltip(Graphics& g, const ToolButton& b) {
    if (!b.tip) return;

    std::wstring label = b.tip;
    if (b.key && *b.key) { label += L"   "; label += b.key; }

    const SIZE m = MeasureText(label, S(12));
    const REAL padX = (REAL)S(9), padY = (REAL)S(5);
    const REAL w = (REAL)m.cx + padX * 2, h = (REAL)m.cy + padY * 2;
    const RECT mon = MonitorFor(POINT{ (g_ov.toolbar.left + g_ov.toolbar.right) / 2,
                                       (g_ov.toolbar.top + g_ov.toolbar.bottom) / 2 });
    REAL x = (b.rc.left + b.rc.right) * 0.5f - w * 0.5f;
    REAL y = (REAL)g_ov.toolbar.top - h - (REAL)S(6);
    if (y < (REAL)(mon.top + S(4))) y = (REAL)(g_ov.toolbar.bottom + S(6));
    x = (REAL)Clampi((int)x, mon.left + S(4),
                     max(mon.left + S(4), mon.right - (int)w - S(4)));

    const Color edge = Argb(210, kPanelEdge);
    FillRoundRect(g, RectF(x, y, w, h), (REAL)S(5), Argb(240, kPanel), &edge, 1.0f);
    DrawTextCrisp(g, label, RECT{ (int)x, (int)y, (int)(x + w), (int)(y + h) },
                  kInk, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP, S(12));
}

void DrawToolbar(HDC dc) {
    Overlay& ov = g_ov;
    if (!ov.toolbarVisible) return;

    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

    const RectF panel((REAL)ov.toolbar.left, (REAL)ov.toolbar.top,
                      (REAL)RectW(ov.toolbar), (REAL)RectH(ov.toolbar));
    const Color edge = Argb(220, kPanelEdge);
    FillRoundRect(g, panel, (REAL)S(8), Argb(244, kPanel), &edge, 1.0f);

    const ToolButton* hovered = nullptr;

    for (const ToolButton& b : ov.buttons) {
        const RectF rc((REAL)b.rc.left, (REAL)b.rc.top,
                       (REAL)RectW(b.rc), (REAL)RectH(b.rc));

        if (IsSep(b.cmd)) {
            SolidBrush line(Argb(135, kPanelEdge));
            g.FillRectangle(&line, RectF(rc.X + rc.Width * 0.5f - 0.5f,
                                         rc.Y + rc.Height * 0.16f,
                                         1.0f, rc.Height * 0.68f));
            continue;
        }

        const bool hot = (ov.hotCmd == b.cmd);
        if (hot) hovered = &b;

        if (IsSwatch(b.cmd)) {
            const COLORREF sc  = kSwatches[SwatchIndex(b.cmd)];
            const bool     on  = (ov.color == sc);
            const REAL     rad = rc.Width * 0.30f;
            const PointF   c(rc.X + rc.Width * 0.5f, rc.Y + rc.Height * 0.5f);
            const REAL     ro  = rad + (REAL)S(3);

            if (on || hot) {
                Pen ring(on ? Argb(235, kInk) : Argb(85, kInk), on ? max(1.5f, (REAL)S(2) * 0.85f) : 1.0f);
                g.DrawEllipse(&ring, RectF(c.X - ro, c.Y - ro, ro * 2, ro * 2));
            }
            SolidBrush fill(Opaque(sc));
            g.FillEllipse(&fill, RectF(c.X - rad, c.Y - rad, rad * 2, rad * 2));
            if (sc == RGB(255, 255, 255) || sc == RGB(24, 25, 30)) {
                Pen outline(Argb(130, kInkMuted), 1.0f);
                g.DrawEllipse(&outline, RectF(c.X - rad, c.Y - rad, rad * 2, rad * 2));
            }
            continue;
        }

        const Tool asTool = CmdToTool(b.cmd);
        const bool active = (asTool != Tool::COUNT && ov.tool == asTool);
        const bool dimmed = (b.cmd == Cmd::Undo && ov.shapes.empty()) ||
                            (b.cmd == Cmd::Redo && ov.undone.empty());

        if (active)                   FillRoundRect(g, rc, (REAL)S(6), Argb(58, g_cfg.accent));
        else if (hot && !dimmed)      FillRoundRect(g, rc, (REAL)S(6), Argb(155, kHover));

        Color ink = active ? Opaque(g_cfg.accent) : Opaque(kInk);
        if (dimmed)                   ink = Argb(70, kInkMuted);
        else if (b.cmd == Cmd::Close) ink = Argb(238, RGB(255, 118, 118));

        if (b.cmd == Cmd::Width) {
            const REAL rad = max(1.6f, (REAL)ov.width * ov.scale * 0.8f + 1.1f);
            SolidBrush dot(Opaque(kInk));
            g.FillEllipse(&dot, RectF(rc.X + rc.Width * 0.5f - rad,
                                      rc.Y + rc.Height * 0.5f - rad, rad * 2, rad * 2));
        } else {
            const REAL inset = rc.Width * 0.28f;
            DrawCmdIcon(g, b.cmd, RectF(rc.X + inset * 0.5f, rc.Y + inset * 0.5f,
                                        rc.Width - inset, rc.Height - inset), ink);
        }
    }

    if (hovered) DrawTooltip(g, *hovered);
}

} // namespace

// ---------------------------------------------------------------------------
//  Badges, magnifier, hint
// ---------------------------------------------------------------------------
namespace {

const int kZoomCells = 15;                 // odd, so there is a true centre cell

int  ZoomCell()  { return S(8); }
int  ZoomBox()   { return ZoomCell() * kZoomCells; }

bool MagnifierVisible() {
    const Overlay& ov = g_ov;
    if (!ov.magnifier || ov.editing) return false;
    if (ov.phase == Phase::Idle)     return true;
    if (ov.phase == Phase::Dragging) return true;
    return ov.mouseDown && ov.grip != Grip::None && ov.grip != Grip::Inside;
}

RECT MagnifierRect() {
    const Overlay& ov = g_ov;
    const int pad  = S(7);
    const int rowH = S(17);
    const int w    = ZoomBox() + pad * 2;
    const int h    = ZoomBox() + pad * 2 + rowH * 2;

    RECT mon = ov.curMonitor;
    if (RectEmptyish(mon)) mon = RECT{ 0, 0, ov.cap.w, ov.cap.h };

    int x = ov.cursor.x + S(20);
    int y = ov.cursor.y + S(20);
    if (x + w > mon.right  - S(6)) x = ov.cursor.x - S(20) - w;
    if (y + h > mon.bottom - S(6)) y = ov.cursor.y - S(20) - h;
    x = Clampi(x, mon.left + S(6), max(mon.left + S(6), mon.right  - w - S(6)));
    y = Clampi(y, mon.top  + S(6), max(mon.top  + S(6), mon.bottom - h - S(6)));
    return RECT{ x, y, x + w, y + h };
}

RECT PillRect(Graphics&, const std::wstring& text, int x, int y, int fontPx) {
    const SIZE m = MeasureText(text, fontPx);
    const int  padX = S(9), padY = S(5);
    return RECT{ x, y, x + m.cx + padX * 2, y + m.cy + padY * 2 };
}

void DrawPill(Graphics& g, const RECT& box, const std::wstring& text, int fontPx,
              COLORREF ink = kInk, int bgAlpha = 236) {
    const Color edge = Argb(190, kPanelEdge);
    FillRoundRect(g, RectF((REAL)box.left, (REAL)box.top,
                           (REAL)RectW(box), (REAL)RectH(box)),
                  (REAL)S(5), Argb(bgAlpha, kPanel), &edge, 1.0f);
    DrawTextCrisp(g, text, box, ink,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP, fontPx);
}

RECT SizeBadgeRect(Graphics& g, const RECT& sel) {
    wchar_t buf[64];
    wsprintfW(buf, L"%d \u00D7 %d", RectW(sel), RectH(sel));
    RECT r = PillRect(g, buf, 0, 0, S(12));
    const int w = RectW(r), h = RectH(r);
    const RECT mon = MonitorFor(POINT{ sel.left, sel.top });

    int x = sel.left;
    int y = sel.top - h - S(7);
    if (y < mon.top + S(4)) y = sel.top + S(7);
    x = Clampi(x, mon.left + S(4), max(mon.left + S(4), mon.right - w - S(4)));
    return RECT{ x, y, x + w, y + h };
}

void DrawMagnifier(HDC dc) {
    const Overlay& ov  = g_ov;
    const RECT     box = MagnifierRect();
    const int      pad = S(7);
    const int      zb  = ZoomBox();
    const int      cell = ZoomCell();

    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

    const Color edge = Argb(215, kPanelEdge);
    FillRoundRect(g, RectF((REAL)box.left, (REAL)box.top,
                           (REAL)RectW(box), (REAL)RectH(box)),
                  (REAL)S(7), Argb(244, kPanel), &edge, 1.0f);

    const int zx = box.left + pad, zy = box.top + pad;
    const int half = kZoomCells / 2;

    HDC raw = g.GetHDC();
    if (raw) {
        SetStretchBltMode(raw, COLORONCOLOR);          // nearest neighbour
        StretchBlt(raw, zx, zy, zb, zb, ov.cap.dc,
                   ov.cursor.x - half, ov.cursor.y - half,
                   kZoomCells, kZoomCells, SRCCOPY);
        g.ReleaseHDC(raw);
    }

    Pen grid(Argb(38, RGB(255, 255, 255)), 1.0f);
    for (int i = 1; i < kZoomCells; ++i) {
        g.DrawLine(&grid, PointF((REAL)(zx + i * cell), (REAL)zy),
                          PointF((REAL)(zx + i * cell), (REAL)(zy + zb)));
        g.DrawLine(&grid, PointF((REAL)zx, (REAL)(zy + i * cell)),
                          PointF((REAL)(zx + zb), (REAL)(zy + i * cell)));
    }

    const int cx = zx + half * cell, cy = zy + half * cell;
    Pen ring(Opaque(g_cfg.accent), max(1.5f, (REAL)S(2) * 0.9f));
    g.DrawRectangle(&ring, RectF((REAL)cx, (REAL)cy, (REAL)cell, (REAL)cell));
    Pen frame(Argb(70, kPanelEdge), 1.0f);
    g.DrawRectangle(&frame, RectF((REAL)zx, (REAL)zy, (REAL)zb, (REAL)zb));

    const uint32_t rgb = ov.cap.at(ov.cursor.x, ov.cursor.y);
    wchar_t line1[64], line2[64];
    wsprintfW(line1, L"%d, %d", ov.cursor.x + ov.cap.ox, ov.cursor.y + ov.cap.oy);
    wsprintfW(line2, L"#%02X%02X%02X",
              (int)((rgb >> 16) & 0xFF), (int)((rgb >> 8) & 0xFF), (int)(rgb & 0xFF));

    const int  rowH = S(17);
    const REAL chip = (REAL)S(9);
    const REAL chipY = (REAL)(zy + zb + rowH) + (rowH - chip) / 2;

    SolidBrush chipBrush(Color(255, (BYTE)((rgb >> 16) & 0xFF),
                                    (BYTE)((rgb >> 8) & 0xFF), (BYTE)(rgb & 0xFF)));
    g.FillRectangle(&chipBrush, RectF((REAL)zx, chipY, chip, chip));
    Pen chipEdge(Argb(120, kPanelEdge), 1.0f);
    g.DrawRectangle(&chipEdge, RectF((REAL)zx + 0.5f, chipY + 0.5f, chip - 1, chip - 1));

    const UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP;
    DrawTextCrisp(g, line1, RECT{ zx, zy + zb, zx + zb, zy + zb + rowH },
                  kInkMuted, flags, S(12));
    DrawTextCrisp(g, line2, RECT{ zx + (int)chip + S(6), zy + zb + rowH,
                                  zx + zb, zy + zb + rowH * 2 },
                  kInk, flags, S(12), FW_SEMIBOLD);
}

} // namespace

// ---------------------------------------------------------------------------
//  Selection chrome + frame composition
// ---------------------------------------------------------------------------
namespace {

void EnsureBackBuffer() {
    Overlay& ov = g_ov;
    if (ov.bbDC && ov.bbW == ov.cap.w && ov.bbH == ov.cap.h) return;

    if (ov.bbDC)  { DeleteDC(ov.bbDC);         ov.bbDC  = nullptr; }
    if (ov.bbBmp) { DeleteObject(ov.bbBmp);    ov.bbBmp = nullptr; }

    // A DIB rather than a compatible bitmap: the base layer is composed by
    // writing pixels directly (see ComposeBase), which needs addressable bits.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = ov.cap.w;
    bi.bmiHeader.biHeight      = -ov.cap.h;          // top-down, matching cap
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    ov.bbBmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ov.bbPx  = static_cast<uint32_t*>(bits);
    ov.bbDC  = CreateCompatibleDC(ov.cap.dc);
    if (ov.bbDC && ov.bbBmp) SelectObject(ov.bbDC, ov.bbBmp);
    ov.bbW = ov.cap.w;
    ov.bbH = ov.cap.h;
}

// ---------------------------------------------------------------------------
//  Base layer: the frozen screenshot, dimmed everywhere except one rectangle.
//
//  Doing this with GDI costs three passes over the frame -- copy the capture
//  in, then read-modify-write it with AlphaBlend.  Folding both into a single
//  read-and-write pass makes the opening frame markedly cheaper, and the blend
//  itself becomes three table lookups per pixel.
// ---------------------------------------------------------------------------
struct DimTable {
    uint32_t r[256], g[256], b[256];
    int      alpha = -1;
    COLORREF color = 0;
};

const DimTable& DimLut(int alpha, COLORREF c) {
    static DimTable a, b;                    // the full dim and the soft dim
    DimTable& t = (alpha == g_cfg.dimAlpha) ? a : b;
    if (t.alpha == alpha && t.color == c) return t;

    const int inv = 255 - Clampi(alpha, 0, 255);
    const int ar = GetRValue(c) * alpha, ag = GetGValue(c) * alpha, ab = GetBValue(c) * alpha;
    for (int v = 0; v < 256; ++v) {
        t.r[v] = (uint32_t)((v * inv + ar) / 255) << 16;
        t.g[v] = (uint32_t)((v * inv + ag) / 255) << 8;
        t.b[v] = (uint32_t)((v * inv + ab) / 255);
    }
    t.alpha = alpha;
    t.color = c;
    return t;
}

void DimRun(uint32_t* dst, const uint32_t* src, int n, const DimTable& t) {
    for (int i = 0; i < n; ++i) {
        const uint32_t p = src[i];
        dst[i] = t.r[(p >> 16) & 0xFF] | t.g[(p >> 8) & 0xFF] | t.b[p & 0xFF];
    }
}

// `keep` is the one rectangle drawn at a different strength: the selection
// (fully bright) or the hovered window (lightly dimmed).
void ComposeBase(const RECT& area, const RECT* keep, int keepAlpha) {
    Overlay& ov = g_ov;
    const DimTable& full = DimLut(g_cfg.dimAlpha, kDim);
    const DimTable& soft = DimLut(keepAlpha, kDim);

    GdiFlush();                              // pending GDI ops touch these bits

    for (int y = area.top; y < area.bottom; ++y) {
        const uint32_t* src = ov.cap.px + (size_t)y * ov.cap.w;
        uint32_t*       dst = ov.bbPx   + (size_t)y * ov.bbW;

        int lo = area.right, hi = area.right;
        if (keep && y >= keep->top && y < keep->bottom) {
            lo = Clampi(keep->left,  area.left, area.right);
            hi = Clampi(keep->right, area.left, area.right);
        }

        if (lo > area.left) DimRun(dst + area.left, src + area.left, lo - area.left, full);
        if (hi > lo) {
            if (keepAlpha <= 0) memcpy(dst + lo, src + lo, (size_t)(hi - lo) * 4);
            else                DimRun(dst + lo, src + lo, hi - lo, soft);
        }
        if (area.right > hi) DimRun(dst + hi, src + hi, area.right - hi, full);
    }
}

void DrawHandles(HDC dc, const RECT& sel) {
    const REAL r = max(3.0f, (REAL)S(5));
    if (RectW(sel) < S(20) || RectH(sel) < S(20)) return;

    const int mx = (sel.left + sel.right) / 2;
    const int my = (sel.top + sel.bottom) / 2;
    const POINT pts[8] = {
        { sel.left, sel.top }, { mx, sel.top }, { sel.right, sel.top },
        { sel.right, my },
        { sel.right, sel.bottom }, { mx, sel.bottom }, { sel.left, sel.bottom },
        { sel.left, my }
    };

    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    SolidBrush fill(Opaque(RGB(255, 255, 255)));
    Pen        edge(Opaque(g_cfg.accent), max(1.4f, (REAL)S(2) * 0.8f));
    for (const POINT& p : pts) {
        const RectF box((REAL)p.x - r, (REAL)p.y - r, r * 2, r * 2);
        g.FillEllipse(&fill, box);
        g.DrawEllipse(&edge, box);
    }
}

void DrawCaret(HDC dc) {
    const Overlay& ov = g_ov;
    if (!ov.editing || ov.editIndex >= ov.shapes.size() || !ov.caretOn) return;

    const Shape& s = ov.shapes[ov.editIndex];
    if (s.pts.empty()) return;

    const SIZE m = MeasureAnnotationText(s.text, s.width);
    const int  h = TextFontSize(s.width);
    RECT caret{ s.pts[0].x + m.cx + 1, s.pts[0].y + 2,
                s.pts[0].x + m.cx + 1 + max(1, S(2)), s.pts[0].y + h };
    FillSolid(dc, caret, s.color);
}

void DrawHint(HDC dc) {
    const Overlay& ov = g_ov;
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

    const std::wstring text =
        L"Drag to select     \u00B7     Click to grab a window     "
        L"\u00B7     C copies the colour     \u00B7     Esc cancels";

    RECT r = PillRect(g, text, 0, 0, S(12));
    const int w = RectW(r), h = RectH(r);
    RECT mon = ov.curMonitor;
    if (RectEmptyish(mon)) mon = RECT{ 0, 0, ov.cap.w, ov.cap.h };

    const int x = (mon.left + mon.right) / 2 - w / 2;
    const int y = mon.bottom - h - S(48);
    DrawPill(g, RECT{ x, y, x + w, y + h }, text, S(12), kInkMuted, 210);
}

} // namespace

RECT ChromeBounds() {
    const Overlay& ov = g_ov;
    RECT out{ 0, 0, 0, 0 };
    bool any = false;

    auto add = [&](const RECT& r) {
        if (RectEmptyish(r)) return;
        if (!any) { out = r; any = true; }
        else      { UnionRect(&out, &out, &r); }
    };

    if (ov.hasSel) {
        RECT s = ov.sel;
        InflateRect(&s, S(12), S(12));
        s.top    -= S(30);                      // size badge
        s.right   = max(s.right, ov.sel.left + S(170));
        add(s);
    }
    if (ov.hasSnap && !ov.hasSel) {
        RECT s = ov.snap;
        InflateRect(&s, S(4), S(4));
        add(s);
    }
    if (ov.toolbarVisible) {
        RECT t = ov.toolbar;
        InflateRect(&t, S(140), S(38));          // room for the tooltip
        add(t);
    }
    if (MagnifierVisible()) add(MagnifierRect());

    if (ov.phase == Phase::Idle) {               // hint pill
        RECT mon = ov.curMonitor;
        if (RectEmptyish(mon)) mon = RECT{ 0, 0, ov.cap.w, ov.cap.h };
        const int w = S(560), h = S(30);
        const int x = (mon.left + mon.right) / 2 - w / 2;
        add(RECT{ x, mon.bottom - h - S(54), x + w, mon.bottom - S(42) });
    }

    if (any) {
        RECT screen{ 0, 0, ov.cap.w, ov.cap.h };
        IntersectRect(&out, &out, &screen);
    }
    return out;
}

void PaintOverlay(HDC hdc, const RECT& dirty) {
    Overlay& ov = g_ov;
    if (!ov.cap.valid() || RectEmptyish(dirty)) return;

    // Composing straight into the window DC was measured and is markedly
    // slower and jumpier: every operation then goes through the compositor's
    // surface.  Memory first, one blit out, always.
    EnsureBackBuffer();
    HDC dc = ov.bbDC ? ov.bbDC : hdc;

    HRGN clip = CreateRectRgn(dirty.left, dirty.top, dirty.right, dirty.bottom);
    SelectClipRgn(dc, clip);

    const int dw = RectW(dirty), dh = RectH(dirty);

    // --- base layer: capture + dim, in one pass ----------------------------
    // The selection stays bright; failing that, a hovered window is dimmed
    // more gently than the rest.  Only one of the two is ever live.
    if (ov.bbPx && dc == ov.bbDC) {
        const RECT* keep      = nullptr;
        int         keepAlpha = 0;
        if (ov.hasSel)            { keep = &ov.sel;  keepAlpha = 0; }
        else if (ov.hasSnap)      { keep = &ov.snap; keepAlpha = g_cfg.dimAlpha / 3; }
        ComposeBase(dirty, keep, keepAlpha);
    } else {
        BitBlt(dc, dirty.left, dirty.top, dw, dh, ov.cap.dc, dirty.left, dirty.top, SRCCOPY);
        RECT inter;
        if (ov.hasSel && IntersectRect(&inter, &dirty, &ov.sel)) {
            FillAlpha(dc, RECT{ dirty.left, dirty.top,    dirty.right, inter.top    }, kDim, g_cfg.dimAlpha);
            FillAlpha(dc, RECT{ dirty.left, inter.bottom, dirty.right, dirty.bottom }, kDim, g_cfg.dimAlpha);
            FillAlpha(dc, RECT{ dirty.left, inter.top,    inter.left,  inter.bottom }, kDim, g_cfg.dimAlpha);
            FillAlpha(dc, RECT{ inter.right, inter.top,   dirty.right, inter.bottom }, kDim, g_cfg.dimAlpha);
        } else {
            FillAlpha(dc, dirty, kDim, g_cfg.dimAlpha);
        }
    }

    // --- crosshair guides ---------------------------------------------------
    if (ov.phase == Phase::Idle && !ov.hasSel) {
        const int t = S(1);
        FillAlpha(dc, RECT{ 0, ov.cursor.y, ov.cap.w, ov.cursor.y + t },
                  g_cfg.accent, 120);
        FillAlpha(dc, RECT{ ov.cursor.x, 0, ov.cursor.x + t, ov.cap.h },
                  g_cfg.accent, 120);
    }

    // --- window snap preview ------------------------------------------------
    if (ov.hasSnap && !ov.hasSel) {
        if (!(ov.bbPx && dc == ov.bbDC)) {          // ComposeBase already did it
            RECT s;
            if (IntersectRect(&s, &dirty, &ov.snap)) {
                BitBlt(dc, s.left, s.top, RectW(s), RectH(s), ov.cap.dc, s.left, s.top, SRCCOPY);
                FillAlpha(dc, s, kDim, g_cfg.dimAlpha / 3);
            }
        }
        FrameLines(dc, ov.snap, g_cfg.accent, S(2));
    }

    // --- annotations --------------------------------------------------------
    if (ov.hasSel) {
        RECT ann;
        if (IntersectRect(&ann, &dirty, &ov.sel))
            DrawShapes(dc, ov.cap, ov.shapes, ov.drawing ? &ov.live : nullptr, 0, 0, ann);
        DrawCaret(dc);

        FrameLines(dc, ov.sel, g_cfg.accent, S(1));
        if (ov.phase == Phase::Ready) DrawHandles(dc, ov.sel);

        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        wchar_t buf[64];
        wsprintfW(buf, L"%d \u00D7 %d", RectW(ov.sel), RectH(ov.sel));
        DrawPill(g, SizeBadgeRect(g, ov.sel), buf, S(12));
    }

    if (ov.phase == Phase::Idle && !ov.hasSel) DrawHint(dc);

    DrawToolbar(dc);
    if (MagnifierVisible()) DrawMagnifier(dc);

    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);

    if (dc != hdc) BitBlt(hdc, dirty.left, dirty.top, dw, dh, dc, dirty.left, dirty.top, SRCCOPY);
}
