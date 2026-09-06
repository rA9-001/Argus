// ---------------------------------------------------------------------------
//  Internal state shared between the overlay's input and painting halves.
// ---------------------------------------------------------------------------
#pragma once
#include "app.h"

enum class Phase { Idle, Dragging, Ready };

enum class Grip {
    None = 0, Inside,
    N, S, E, W, NE, NW, SE, SW
};

// Toolbar item identifiers.  Order here is the order on screen.
enum class Cmd {
    None = 0,
    ToolSelect, ToolPen, ToolLine, ToolArrow, ToolRect, ToolEllipse,
    ToolHighlight, ToolText, ToolCounter, ToolPixelate,
    Sep1,
    Color0, Color1, Color2, Color3, Color4, Color5,
    Width,
    Sep2,
    Undo, Redo,
    Sep3,
    Copy, Save, Close
};

struct ToolButton {
    Cmd          cmd;
    const wchar_t* tip;
    const wchar_t* key;       // shortcut shown in the tooltip
    RECT         rc;          // filled in by the layout pass
};

struct Overlay {
    HWND        hwnd   = nullptr;
    HINSTANCE   hInst  = nullptr;

    Capture       cap;
    WindowTargets targets;

    // --- geometry (client coords == capture pixel coords) ------------------
    RECT   sel{};
    bool   hasSel = false;
    RECT   snap{};
    bool   hasSnap = false;
    POINT  cursor{};
    RECT   curMonitor{};
    float  scale = 1.0f;          // DPI scale of the monitor under the cursor

    // --- interaction -------------------------------------------------------
    Phase  phase   = Phase::Idle;
    Grip   grip    = Grip::None;
    Grip   hotGrip = Grip::None;
    POINT  anchor{};              // drag origin
    RECT   dragBase{};            // selection at drag start
    bool   mouseDown = false;
    bool   moved     = false;

    // --- annotation --------------------------------------------------------
    Tool               tool  = Tool::Select;
    COLORREF           color = RGB(255, 61, 61);
    int                width = 3;
    std::vector<Shape> shapes;
    std::vector<Shape> undone;
    Shape              live;
    bool               drawing = false;
    int                counter = 1;

    bool   editing    = false;    // inline text entry
    size_t editIndex  = 0;
    bool   caretOn    = true;

    // --- chrome ------------------------------------------------------------
    bool   magnifier  = true;
    Cmd    hotCmd     = Cmd::None;
    Cmd    pressedCmd = Cmd::None;
    std::vector<ToolButton> buttons;
    RECT   toolbar{};
    bool   toolbarVisible = false;
    RECT   lastChrome{};          // union of chrome painted last frame

    // --- back buffer -------------------------------------------------------
    HDC       bbDC  = nullptr;
    HBITMAP   bbBmp = nullptr;
    uint32_t* bbPx  = nullptr;     // back buffer is a DIB, so it is writable
    int       bbW = 0, bbH = 0;

    void reset();
};

extern Overlay g_ov;

// paint.cpp
void  PaintInit();
void  PaintShutdown();
void  PaintOverlay(HDC hdc, const RECT& dirty);
void  LayoutToolbar();
Cmd   HitToolbar(POINT p);
RECT  ChromeBounds();                       // everything painted outside `sel`
bool  RenderExport(const RECT& region, Capture& out);
int   TextFontSize(int strokeWidth);
SIZE  MeasureAnnotationText(const std::wstring& text, int strokeWidth);

// overlay.cpp
void  OverlayDamage(const RECT& r);
void  OverlayRefreshChrome();
