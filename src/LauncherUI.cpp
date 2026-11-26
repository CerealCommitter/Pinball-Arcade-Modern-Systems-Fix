// Reusable CerealCommitter launcher window: logo on a checkerboard panel at the left, the game's
// settings on a dark panel at the right (design "option B", see design/launcher-options.html).
//
// Everything is drawn by hand (GDI+ for shapes and the logo, GDI for text) so it looks the same
// on every Windows version. Drop-downs are owner-drawn buttons that open owner-drawn menus.
#include "LauncherUI.h"

#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {
using namespace Gdiplus;

// ---- palette (from the CerealCommitter logo) -----------------------------------------
const Color kBg(0x17, 0x17, 0x1C);
const Color kSide(0x23, 0x11, 0x3A);
const Color kChecker(0x2C, 0x17, 0x49);
const Color kPink(0xFF, 0x2F, 0xA6);
const Color kGreen(0x7D, 0xFF, 0x3A);
const Color kPurple(0x7A, 0x3F, 0xF2);
const Color kPurpleDown(0x63, 0x2E, 0xD6);
const Color kField(0x22, 0x22, 0x29);
const Color kFieldDown(0x2B, 0x2B, 0x34);
const Color kFieldBorder(0x3A, 0x3A, 0x46);
const Color kQuitBorder(0x4A, 0x4A, 0x58);
const COLORREF kText = RGB(0xEC, 0xEC, 0xF1);
const COLORREF kSubText = RGB(0x9B, 0x9B, 0xA8);
const COLORREF kLabel = RGB(0xB9, 0xB9, 0xC6);
const COLORREF kHint = RGB(0x80, 0x80, 0x8C);
const COLORREF kQuitText = RGB(0xD6, 0xD6, 0xDE);
const COLORREF kMono = RGB(0x7D, 0xFF, 0x3A);

inline COLORREF Ref(const Color& c) { return RGB(c.GetR(), c.GetG(), c.GetB()); }

// ---- layout in 96-dpi pixels ---------------------------------------------------------
constexpr int kW = 640;
constexpr int kSideW = 230;
constexpr int kDivider = 3;
constexpr int kPad = 28;
constexpr int kLeft = kSideW + kDivider + kPad;  // content x
constexpr int kContentW = kW - kLeft - kPad;
constexpr int kTitleY = 24;
constexpr int kSubY = 58;
constexpr int kChoicesY = 92;
constexpr int kChoiceStep = 68;  // label + field + gap
constexpr int kFieldH = 34;
constexpr int kBtnW = 104, kBtnH = 36, kBtnGap = 10;
constexpr int kLogoW = 200;

enum { IdPlay = IDOK, IdQuit = IDCANCEL, IdCheck = 900, IdSelect0 = 1000, IdMenu0 = 5000 };

struct UI {
    LauncherSpec* spec = nullptr;
    HMODULE res = nullptr;
    HWND hwnd = nullptr;
    int dpi = 96;
    int S(int v) const { return MulDiv(v, dpi, 96); }

    Bitmap* logo = nullptr;
    IStream* logoStream = nullptr;
    HFONT fTitle = nullptr, fSub = nullptr, fLabel = nullptr, fBody = nullptr, fHint = nullptr,
          fBtn = nullptr, fMono = nullptr;
    HBRUSH menuBrush = nullptr;

    std::vector<HWND> selects;
    HWND check = nullptr, play = nullptr, quit = nullptr;
    int checkY = 0, hintY = 0, btnY = 0, height = 0;

    int menuFor = -1;  // which drop-down's menu is open
    int menuWidth = 0;
    bool pressedPlay = false;
    bool done = false;
};

HFONT MakeFont(const UI& ui, int px, int weight, const wchar_t* face = L"Segoe UI") {
    return CreateFontW(-ui.S(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
}

void RoundRect(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r) {
    const REAL d = r * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

void Text(HDC dc, HFONT f, COLORREF c, const std::wstring& s, RECT r, UINT flags) {
    GdiFlush();
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

int TextHeight(HDC dc, HFONT f, const std::wstring& s, int width) {
    HGDIOBJ old = SelectObject(dc, f);
    RECT r = {0, 0, width, 0};
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    return r.bottom;
}

void LoadLogo(UI& ui) {
    HRSRC r = FindResourceW(ui.res, MAKEINTRESOURCEW(IDR_LAUNCHER_LOGO), RT_RCDATA);
    if (!r) return;
    HGLOBAL h = LoadResource(ui.res, r);
    const DWORD size = SizeofResource(ui.res, r);
    const void* data = h ? LockResource(h) : nullptr;
    if (!data || !size) return;
    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!copy) return;
    memcpy(GlobalLock(copy), data, size);
    GlobalUnlock(copy);
    if (CreateStreamOnHGlobal(copy, TRUE, &ui.logoStream) != S_OK) {
        GlobalFree(copy);
        return;
    }
    ui.logo = Bitmap::FromStream(ui.logoStream);
    if (ui.logo && ui.logo->GetLastStatus() != Ok) {
        delete ui.logo;
        ui.logo = nullptr;
    }
}

// ---- painting --------------------------------------------------------------------------
void PaintWindow(UI& ui, HDC dc, const RECT& rc) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    SolidBrush bg(kBg);
    g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);

    // Left panel: purple-and-black checkerboard from the logo.
    const int side = ui.S(kSideW);
    SolidBrush sideBrush(kSide), checker(kChecker);
    g.FillRectangle(&sideBrush, 0, 0, side, rc.bottom);
    const int cell = ui.S(14);
    for (int y = 0; y < rc.bottom; y += cell)
        for (int x = 0; x < side; x += cell)
            if (((x / cell) + (y / cell)) % 2 == 0)
                g.FillRectangle(&checker, x, y, (x + cell > side ? side - x : cell), cell);
    SolidBrush pink(kPink);
    g.FillRectangle(&pink, side, 0, ui.S(kDivider), rc.bottom);

    // Logo, with a small "> ship()" under it, centred in the panel.
    const int monoH = ui.S(16), gap = ui.S(10);
    int logoW = 0, logoH = 0;
    if (ui.logo) {
        logoW = ui.S(kLogoW);
        logoH = MulDiv(logoW, ui.logo->GetHeight(), ui.logo->GetWidth());
    }
    const int groupH = logoH + gap + monoH;
    const int top = (rc.bottom - groupH) / 2;
    if (ui.logo) {
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.DrawImage(ui.logo, (side - logoW) / 2, top, logoW, logoH);
    }
    g.Flush(FlushIntentionSync);
    Text(dc, ui.fMono, kMono, L"> ship()", RECT{0, top + logoH + gap, side, top + groupH + gap},
         DT_CENTER | DT_SINGLELINE);

    // Right panel texts.
    const int x = ui.S(kLeft), w = ui.S(kContentW);
    std::wstring spaced;
    for (size_t i = 0; i < ui.spec->gameTitle.size(); ++i) {
        if (i) spaced += L' ';
        spaced += ui.spec->gameTitle[i];
    }
    Text(dc, ui.fTitle, kText, spaced, RECT{x, ui.S(kTitleY), x + w, ui.S(kTitleY + 34)},
         DT_LEFT | DT_SINGLELINE);
    Text(dc, ui.fSub, kSubText, ui.spec->subtitle, RECT{x, ui.S(kSubY), x + w, ui.S(kSubY + 20)},
         DT_LEFT | DT_SINGLELINE);
    for (size_t i = 0; i < ui.spec->choices.size(); ++i) {
        const int y = ui.S(kChoicesY + (int)i * kChoiceStep);
        Text(dc, ui.fLabel, kLabel, ui.spec->choices[i].label, RECT{x, y, x + w, y + ui.S(20)},
             DT_LEFT | DT_SINGLELINE);
    }
    Text(dc, ui.fHint, kHint, ui.spec->hintText,
         RECT{x + ui.S(26), ui.hintY, x + w, ui.btnY}, DT_LEFT | DT_WORDBREAK);
}

void DrawSelect(UI& ui, const DRAWITEMSTRUCT& d, int index) {
    const LauncherChoice& c = ui.spec->choices[index];
    const RECT& r = d.rcItem;
    Graphics g(d.hDC);
    SolidBrush bg(kBg);
    g.FillRectangle(&bg, r.left, r.top, r.right - r.left, r.bottom - r.top);
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    const bool open = ui.menuFor == index || (d.itemState & ODS_SELECTED);
    const bool focus = (d.itemState & ODS_FOCUS) != 0;
    GraphicsPath p;
    RoundRect(p, 0.5f, 0.5f, (REAL)(r.right - r.left) - 1, (REAL)(r.bottom - r.top) - 1,
              (REAL)ui.S(6));
    SolidBrush fill(open ? kFieldDown : kField);
    g.FillPath(&fill, &p);
    Pen border(focus || open ? kPink : kFieldBorder, (REAL)ui.S(1));
    g.DrawPath(&border, &p);

    // Chevron.
    const REAL cx = (REAL)(r.right - ui.S(18)), cy = (REAL)(r.bottom - r.top) / 2;
    const REAL s = (REAL)ui.S(4);
    Pen chev(kPink, (REAL)ui.S(2));
    chev.SetStartCap(LineCapRound);
    chev.SetEndCap(LineCapRound);
    PointF pts[] = {{cx - s, cy - s / 2}, {cx, cy + s / 2}, {cx + s, cy - s / 2}};
    g.DrawLines(&chev, pts, 3);

    const std::wstring& text =
        (c.selected >= 0 && c.selected < (int)c.items.size()) ? c.items[c.selected] : L"";
    g.Flush(FlushIntentionSync);
    Text(d.hDC, ui.fBody, kText, text,
         RECT{r.left + ui.S(12), r.top, r.right - ui.S(34), r.bottom},
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void DrawCheck(UI& ui, const DRAWITEMSTRUCT& d) {
    const RECT& r = d.rcItem;
    Graphics g(d.hDC);
    SolidBrush bg(kBg);
    g.FillRectangle(&bg, r.left, r.top, r.right - r.left, r.bottom - r.top);
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    const REAL box = (REAL)ui.S(16);
    const REAL bx = 1, by = ((REAL)(r.bottom - r.top) - box) / 2;
    GraphicsPath p;
    RoundRect(p, bx, by, box, box, (REAL)ui.S(4));
    if (ui.spec->checked) {
        SolidBrush green(kGreen);
        g.FillPath(&green, &p);
        Pen tick(kBg, (REAL)ui.S(2));
        tick.SetStartCap(LineCapRound);
        tick.SetEndCap(LineCapRound);
        tick.SetLineJoin(LineJoinRound);
        PointF pts[] = {{bx + box * 0.25f, by + box * 0.52f},
                        {bx + box * 0.43f, by + box * 0.70f},
                        {bx + box * 0.76f, by + box * 0.32f}};
        g.DrawLines(&tick, pts, 3);
    } else {
        Pen edge(Color(0x6A, 0x6A, 0x78), (REAL)ui.S(1));
        g.DrawPath(&edge, &p);
    }
    if (d.itemState & ODS_FOCUS) {
        GraphicsPath ring;
        RoundRect(ring, bx - 2, by - 2, box + 4, box + 4, (REAL)ui.S(5));
        Pen pinkPen(kPink, 1.5f);
        g.DrawPath(&pinkPen, &ring);
    }
    g.Flush(FlushIntentionSync);
    Text(d.hDC, ui.fBody, kText, ui.spec->checkboxText,
         RECT{r.left + ui.S(26), r.top, r.right, r.bottom}, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void DrawButton(UI& ui, const DRAWITEMSTRUCT& d, bool primary) {
    const RECT& r = d.rcItem;
    Graphics g(d.hDC);
    SolidBrush bg(kBg);
    g.FillRectangle(&bg, r.left, r.top, r.right - r.left, r.bottom - r.top);
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    const bool down = (d.itemState & ODS_SELECTED) != 0;
    const bool focus = (d.itemState & ODS_FOCUS) != 0;
    GraphicsPath p;
    RoundRect(p, 0.5f, 0.5f, (REAL)(r.right - r.left) - 1, (REAL)(r.bottom - r.top) - 1,
              (REAL)ui.S(6));
    if (primary) {
        SolidBrush fill(down ? kPurpleDown : kPurple);
        g.FillPath(&fill, &p);
        if (focus) {
            Pen ring(Color(0xD9, 0xC8, 0xFF), (REAL)ui.S(2));
            GraphicsPath inner;
            RoundRect(inner, (REAL)ui.S(2), (REAL)ui.S(2), (REAL)(r.right - r.left - ui.S(4)),
                      (REAL)(r.bottom - r.top - ui.S(4)), (REAL)ui.S(4));
            g.DrawPath(&ring, &inner);
        }
    } else {
        SolidBrush fill(down ? kFieldDown : kBg);
        g.FillPath(&fill, &p);
        Pen border(focus ? kPink : kQuitBorder, (REAL)ui.S(1));
        g.DrawPath(&border, &p);
    }
    wchar_t label[32];
    GetWindowTextW(d.hwndItem, label, 32);
    g.Flush(FlushIntentionSync);
    Text(d.hDC, ui.fBtn, primary ? RGB(255, 255, 255) : kQuitText, label, r,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawMenuItem(UI& ui, const DRAWITEMSTRUCT& d) {
    if (ui.menuFor < 0) return;
    const LauncherChoice& c = ui.spec->choices[ui.menuFor];
    const int idx = (int)d.itemData;
    const bool hot = (d.itemState & ODS_SELECTED) != 0;
    HBRUSH b = CreateSolidBrush(Ref(hot ? kPurple : kField));
    FillRect(d.hDC, &d.rcItem, b);
    DeleteObject(b);
    if (idx == c.selected) {  // mark the current value
        RECT dot = {d.rcItem.left + ui.S(8), (d.rcItem.top + d.rcItem.bottom) / 2 - ui.S(3),
                    d.rcItem.left + ui.S(14), (d.rcItem.top + d.rcItem.bottom) / 2 + ui.S(3)};
        HBRUSH pb = CreateSolidBrush(Ref(hot ? Color(255, 255, 255) : kPink));
        HGDIOBJ oldB = SelectObject(d.hDC, pb);
        HGDIOBJ oldP = SelectObject(d.hDC, GetStockObject(NULL_PEN));
        Ellipse(d.hDC, dot.left, dot.top, dot.right, dot.bottom);
        SelectObject(d.hDC, oldB);
        SelectObject(d.hDC, oldP);
        DeleteObject(pb);
    }
    if (idx >= 0 && idx < (int)c.items.size())
        Text(d.hDC, ui.fBody, hot ? RGB(255, 255, 255) : kText, c.items[idx],
             RECT{d.rcItem.left + ui.S(22), d.rcItem.top, d.rcItem.right - ui.S(8),
                  d.rcItem.bottom},
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void OpenMenu(UI& ui, int index) {
    LauncherChoice& c = ui.spec->choices[index];
    HMENU m = CreatePopupMenu();
    for (size_t j = 0; j < c.items.size(); ++j)
        AppendMenuW(m, MF_OWNERDRAW, IdMenu0 + j, reinterpret_cast<LPCWSTR>(j));
    MENUINFO mi = {sizeof(mi)};
    mi.fMask = MIM_BACKGROUND;
    mi.hbrBack = ui.menuBrush;
    SetMenuInfo(m, &mi);

    RECT br;
    GetWindowRect(ui.selects[index], &br);
    ui.menuFor = index;
    ui.menuWidth = br.right - br.left;
    InvalidateRect(ui.selects[index], nullptr, FALSE);
    const int cmd = (int)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                          br.left, br.bottom + ui.S(2), ui.hwnd, nullptr);
    DestroyMenu(m);
    ui.menuFor = -1;
    if (cmd >= IdMenu0 && cmd < IdMenu0 + (int)c.items.size()) c.selected = cmd - IdMenu0;
    InvalidateRect(ui.selects[index], nullptr, FALSE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ui = reinterpret_cast<UI*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!ui) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            PaintWindow(*ui, mem, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MEASUREITEM: {
            auto* m = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if (m->CtlType == ODT_MENU) {
                // Windows adds room for a check mark; take it off so the menu matches the field.
                m->itemWidth = (UINT)(ui->menuWidth - GetSystemMetrics(SM_CXMENUCHECK) -
                                      ui->S(4));
                m->itemHeight = (UINT)ui->S(30);
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            auto* d = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (d->CtlType == ODT_MENU) {
                DrawMenuItem(*ui, *d);
            } else if (d->CtlID == IdPlay || d->CtlID == IdQuit) {
                DrawButton(*ui, *d, d->CtlID == IdPlay);
            } else if (d->CtlID == IdCheck) {
                DrawCheck(*ui, *d);
            } else if (d->CtlID >= IdSelect0 && d->CtlID < IdSelect0 + ui->selects.size()) {
                DrawSelect(*ui, *d, (int)(d->CtlID - IdSelect0));
            }
            return TRUE;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == IdPlay) {
                ui->pressedPlay = true;
                DestroyWindow(hwnd);
            } else if (id == IdQuit) {
                DestroyWindow(hwnd);
            } else if (id == IdCheck && HIWORD(wp) == BN_CLICKED) {
                ui->spec->checked = !ui->spec->checked;
                InvalidateRect(ui->check, nullptr, FALSE);
            } else if (id >= IdSelect0 && id < IdSelect0 + (int)ui->selects.size() &&
                       HIWORD(wp) == BN_CLICKED) {
                OpenMenu(*ui, id - IdSelect0);
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            ui->done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND MakeButton(UI& ui, int id, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                           x, y, w, h, ui.hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr),
                           nullptr);
}

// Per-monitor DPI for this thread only, so the window is sharp on scaled displays even though
// the game process itself isn't DPI-aware. Loaded dynamically for older Windows versions.
using SetCtxFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
using GetDpiFn = UINT(WINAPI*)();

}  // namespace

bool ShowLauncher(HMODULE resModule, LauncherSpec& spec) {
    UI ui;
    ui.spec = &spec;
    ui.res = resModule;

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto setCtx = reinterpret_cast<SetCtxFn>(GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
    auto getDpi = reinterpret_cast<GetDpiFn>(GetProcAddress(user32, "GetDpiForSystem"));
    DPI_AWARENESS_CONTEXT oldCtx = nullptr;
    if (setCtx) oldCtx = setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (getDpi) ui.dpi = (int)getDpi();

    GdiplusStartupInput gsi;
    ULONG_PTR gdipToken = 0;
    GdiplusStartup(&gdipToken, &gsi, nullptr);
    LoadLogo(ui);

    ui.fTitle = MakeFont(ui, 24, FW_LIGHT);
    ui.fSub = MakeFont(ui, 13, FW_NORMAL);
    ui.fLabel = MakeFont(ui, 13, FW_NORMAL);
    ui.fBody = MakeFont(ui, 14, FW_NORMAL);
    ui.fHint = MakeFont(ui, 12, FW_NORMAL);
    ui.fBtn = MakeFont(ui, 14, FW_SEMIBOLD);
    ui.fMono = MakeFont(ui, 13, FW_NORMAL, L"Consolas");
    ui.menuBrush = CreateSolidBrush(Ref(kField));

    // Vertical layout depends on how many choices there are and how the hint wraps.
    {
        HDC dc = GetDC(nullptr);
        const int n = (int)spec.choices.size();
        ui.checkY = ui.S(kChoicesY + n * kChoiceStep + 2);
        ui.hintY = ui.checkY + ui.S(24);
        const int hintH = TextHeight(dc, ui.fHint, spec.hintText, ui.S(kContentW - 26));
        ui.btnY = ui.hintY + hintH + ui.S(20);
        ui.height = ui.btnY + ui.S(kBtnH) + ui.S(24);
        ReleaseDC(nullptr, dc);
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"CerealCommitterLauncher";
    RegisterClassW(&wc);

    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_CLIPCHILDREN;
    RECT rc = {0, 0, ui.S(kW), ui.height};
    AdjustWindowRect(&rc, style, FALSE);
    const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    ui.hwnd = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_TOPMOST | WS_EX_CONTROLPARENT,
                              wc.lpszClassName, spec.windowTitle.c_str(), style,
                              work.left + (work.right - work.left - ww) / 2,
                              work.top + (work.bottom - work.top - wh) / 2, ww, wh, nullptr,
                              nullptr, wc.hInstance, nullptr);
    bool result = true;  // if no window can be made, just let the game start
    if (ui.hwnd) {
        SetWindowLongPtrW(ui.hwnd, GWLP_USERDATA, (LONG_PTR)&ui);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(ui.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        COLORREF caption = RGB(0x10, 0x10, 0x14);
        DwmSetWindowAttribute(ui.hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));

        HICON icon = nullptr;
        if (ui.logo) {
            Bitmap iconBmp(64, 64, PixelFormat32bppARGB);
            Graphics g(&iconBmp);
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            const int ih = MulDiv(64, ui.logo->GetHeight(), ui.logo->GetWidth());
            g.DrawImage(ui.logo, 0, (64 - ih) / 2, 64, ih);
            iconBmp.GetHICON(&icon);
            SendMessageW(ui.hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
            SendMessageW(ui.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
        }

        const int x = ui.S(kLeft), w = ui.S(kContentW);
        for (size_t i = 0; i < spec.choices.size(); ++i) {
            const int y = ui.S(kChoicesY + (int)i * kChoiceStep + 22);
            ui.selects.push_back(
                MakeButton(ui, IdSelect0 + (int)i, L"", x, y, w, ui.S(kFieldH)));
        }
        ui.check = MakeButton(ui, IdCheck, L"", x, ui.checkY, w, ui.S(22));
        ui.quit = MakeButton(ui, IdQuit, L"Quit",
                             x + w - ui.S(kBtnW) * 2 - ui.S(kBtnGap), ui.btnY, ui.S(kBtnW),
                             ui.S(kBtnH));
        ui.play = MakeButton(ui, IdPlay, L"Play", x + w - ui.S(kBtnW), ui.btnY, ui.S(kBtnW),
                             ui.S(kBtnH));

        ShowWindow(ui.hwnd, SW_SHOW);
        SetForegroundWindow(ui.hwnd);
        SetFocus(ui.play);
        // "Always on top" only so the window isn't hidden behind the launcher at start.
        SetWindowPos(ui.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

        MSG m;
        while (!ui.done && GetMessageW(&m, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(ui.hwnd, &m)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
        result = ui.pressedPlay;
        if (icon) DestroyIcon(icon);
    }

    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    for (HFONT f : {ui.fTitle, ui.fSub, ui.fLabel, ui.fBody, ui.fHint, ui.fBtn, ui.fMono})
        if (f) DeleteObject(f);
    if (ui.menuBrush) DeleteObject(ui.menuBrush);
    delete ui.logo;
    if (ui.logoStream) ui.logoStream->Release();
    GdiplusShutdown(gdipToken);
    if (setCtx && oldCtx) setCtx(oldCtx);
    return result;
}
