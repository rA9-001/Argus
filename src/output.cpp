// ---------------------------------------------------------------------------
//  Clipboard and PNG output.
// ---------------------------------------------------------------------------
#include "app.h"
#include <wincodec.h>
#include <commdlg.h>
#include <shlwapi.h>

namespace {

template <class T>
struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&()     { return &p; }
    T*  operator->()    const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

IWICImagingFactory* Factory() {
    static IWICImagingFactory* f = nullptr;
    if (!f) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&f));
    }
    return f;
}

bool EncodePng(const Capture& img, IStream* stream) {
    IWICImagingFactory* factory = Factory();
    if (!factory || !stream || !img.valid()) return false;

    Com<IWICBitmapEncoder> enc;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) return false;
    if (FAILED(enc->Initialize(stream, WICBitmapEncoderNoCache))) return false;

    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2>         props;
    if (FAILED(enc->CreateNewFrame(&frame, &props)))         return false;
    if (FAILED(frame->Initialize(props.p)))                  return false;
    if (FAILED(frame->SetSize((UINT)img.w, (UINT)img.h)))    return false;
    frame->SetResolution(96.0, 96.0);

    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&fmt)))                 return false;

    const UINT stride = (UINT)img.w * 4;
    if (FAILED(frame->WritePixels((UINT)img.h, stride, stride * (UINT)img.h,
                                  reinterpret_cast<BYTE*>(img.px))))
        return false;

    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(enc->Commit());
}

// Bottom-up 32bpp copy, the layout every clipboard consumer understands.
void FlipInto(const Capture& img, BYTE* dst) {
    const size_t row = (size_t)img.w * 4;
    for (int y = 0; y < img.h; ++y)
        memcpy(dst + row * y, img.px + (size_t)(img.h - 1 - y) * img.w, row);
}

HGLOBAL MakeDib(const Capture& img, bool v5) {
    const size_t hdr   = v5 ? sizeof(BITMAPV5HEADER) : sizeof(BITMAPINFOHEADER);
    const size_t bytes = (size_t)img.w * img.h * 4;

    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, hdr + bytes);
    if (!h) return nullptr;

    BYTE* base = static_cast<BYTE*>(GlobalLock(h));
    if (!base) { GlobalFree(h); return nullptr; }
    memset(base, 0, hdr);

    if (v5) {
        BITMAPV5HEADER* b = reinterpret_cast<BITMAPV5HEADER*>(base);
        b->bV5Size        = sizeof(BITMAPV5HEADER);
        b->bV5Width       = img.w;
        b->bV5Height      = img.h;
        b->bV5Planes      = 1;
        b->bV5BitCount    = 32;
        b->bV5Compression = BI_BITFIELDS;
        b->bV5SizeImage   = (DWORD)bytes;
        b->bV5RedMask     = 0x00FF0000;
        b->bV5GreenMask   = 0x0000FF00;
        b->bV5BlueMask    = 0x000000FF;
        b->bV5AlphaMask   = 0xFF000000;
        b->bV5CSType      = LCS_sRGB;
        b->bV5Intent      = LCS_GM_IMAGES;
    } else {
        BITMAPINFOHEADER* b = reinterpret_cast<BITMAPINFOHEADER*>(base);
        b->biSize        = sizeof(BITMAPINFOHEADER);
        b->biWidth       = img.w;
        b->biHeight      = img.h;
        b->biPlanes      = 1;
        b->biBitCount    = 32;
        b->biCompression = BI_RGB;
        b->biSizeImage   = (DWORD)bytes;
    }

    FlipInto(img, base + hdr);
    GlobalUnlock(h);
    return h;
}

HGLOBAL MakePngBlob(const Capture& img) {
    Com<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, FALSE, &stream))) return nullptr;
    if (!EncodePng(img, stream.p)) return nullptr;

    HGLOBAL src = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.p, &src)) || !src) return nullptr;

    const SIZE_T n = GlobalSize(src);
    HGLOBAL dst = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!dst) return nullptr;

    void* s = GlobalLock(src);
    void* d = GlobalLock(dst);
    if (s && d) memcpy(d, s, n);
    if (s) GlobalUnlock(src);
    if (d) GlobalUnlock(dst);
    return dst;
}

} // namespace

// ---------------------------------------------------------------------------
bool CopyToClipboard(HWND owner, const Capture& img) {
    if (!img.valid()) return false;

    HGLOBAL dib  = MakeDib(img, false);
    HGLOBAL dbv5 = MakeDib(img, true);
    HGLOBAL png  = MakePngBlob(img);

    if (!dib && !dbv5 && !png) return false;

    // Retry briefly: another process may hold the clipboard for a moment.
    bool opened = false;
    for (int i = 0; i < 12 && !opened; ++i) {
        opened = OpenClipboard(owner) != 0;
        if (!opened) Sleep(15);
    }
    if (!opened) {
        if (dib)  GlobalFree(dib);
        if (dbv5) GlobalFree(dbv5);
        if (png)  GlobalFree(png);
        return false;
    }

    EmptyClipboard();
    if (png) {
        const UINT cfPng = RegisterClipboardFormatW(L"PNG");
        if (cfPng && SetClipboardData(cfPng, png)) png = nullptr;
    }
    if (dbv5 && SetClipboardData(CF_DIBV5, dbv5)) dbv5 = nullptr;
    if (dib  && SetClipboardData(CF_DIB,   dib))  dib  = nullptr;
    CloseClipboard();

    if (dib)  GlobalFree(dib);      // anything the clipboard did not take
    if (dbv5) GlobalFree(dbv5);
    if (png)  GlobalFree(png);
    return true;
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return false;
    void* p = GlobalLock(h);
    if (!p) { GlobalFree(h); return false; }
    memcpy(p, text.c_str(), bytes);
    GlobalUnlock(h);

    if (!OpenClipboard(owner)) { GlobalFree(h); return false; }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(h);
    return ok;
}

// ---------------------------------------------------------------------------
bool SavePng(const std::wstring& path, const Capture& img) {
    if (!img.valid() || path.empty()) return false;

    IWICImagingFactory* factory = Factory();
    if (!factory) return false;

    Com<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;

    return EncodePng(img, stream.p);
}

std::wstring BuildSavePath() {
    std::wstring dir = g_cfg.saveDir.empty() ? DefaultSaveDir() : g_cfg.saveDir;
    if (dir.empty()) return L"";
    EnsureDir(dir);

    const std::wstring stem = dir + L"\\Screenshot " + Timestamp();
    std::wstring path = stem + L".png";
    for (int i = 2; i < 1000 && PathFileExistsW(path.c_str()); ++i) {
        wchar_t suffix[16];
        wsprintfW(suffix, L" (%d).png", i);
        path = stem + suffix;
    }
    return path;
}

bool SaveWithDialog(HWND owner, const Capture& img, std::wstring& pathOut) {
    std::wstring dir = g_cfg.saveDir.empty() ? DefaultSaveDir() : g_cfg.saveDir;
    EnsureDir(dir);

    wchar_t file[MAX_PATH * 2];
    const std::wstring initial = L"Screenshot " + Timestamp() + L".png";
    lstrcpynW(file, initial.c_str(), (int)std::size(file));

    OPENFILENAMEW ofn{};
    ofn.lStructSize     = sizeof(ofn);
    ofn.hwndOwner       = owner;
    ofn.lpstrFilter     = L"PNG image\0*.png\0All files\0*.*\0";
    ofn.lpstrFile       = file;
    ofn.nMaxFile        = (DWORD)std::size(file);
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.lpstrDefExt     = L"png";
    ofn.lpstrTitle      = L"Save screenshot";
    ofn.Flags           = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR
                        | OFN_EXPLORER | OFN_ENABLESIZING;

    if (!GetSaveFileNameW(&ofn)) return false;

    pathOut = file;
    return SavePng(pathOut, img);
}

void RevealInExplorer(const std::wstring& path) {
    if (path.empty()) return;
    const std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}
