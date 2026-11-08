#include "ProgressWindow.h"

#include <commctrl.h>

namespace fw::onefile {

namespace {
constexpr wchar_t kClass[] = L"FwOnefileProgress";
constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
constexpr DWORD kExStyle = WS_EX_APPWINDOW;
constexpr int kClientW = 440, kClientH = 104; // at 96 DPI

int px(int v, UINT dpi) { return MulDiv(v, int(dpi), 96); }
} // namespace

bool ProgressWindow::create(HINSTANCE instance, const std::wstring& title)
{
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = kClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), 0));
    RegisterClassExW(&wc);

    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &mi);
    m_hwnd = CreateWindowExW(kExStyle, kClass, title.c_str(), kStyle, mi.rcWork.left, mi.rcWork.top, 200, 100, nullptr, nullptr,
                             instance, this);
    if (!m_hwnd)
        return false;
    m_text = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, m_hwnd, nullptr,
                             instance, nullptr);
    m_bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, m_hwnd, nullptr, instance, nullptr);
    SendMessageW(m_bar, PBM_SETRANGE32, 0, 1000);

    const UINT dpi = GetDpiForWindow(m_hwnd);
    RECT rc{0, 0, px(kClientW, dpi), px(kClientH, dpi)};
    AdjustWindowRectExForDpi(&rc, kStyle, FALSE, kExStyle, dpi);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
    const int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    SetWindowPos(m_hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    layout(dpi);
    ShowWindow(m_hwnd, SW_SHOWNORMAL);
    UpdateWindow(m_hwnd);
    return true;
}

void ProgressWindow::destroy()
{
    if (m_hwnd)
        DestroyWindow(m_hwnd);
    m_hwnd = m_text = m_bar = nullptr;
    if (m_font)
        DeleteObject(m_font);
    m_font = nullptr;
}

void ProgressWindow::setText(const std::wstring& text)
{
    if (m_text && text != m_lastText) {
        m_lastText = text;
        SetWindowTextW(m_text, text.c_str());
    }
}

void ProgressWindow::setProgress(int permille)
{
    if (!m_bar || permille == m_lastPermille)
        return;
    m_lastPermille = permille;
    const LONG_PTR style = GetWindowLongPtrW(m_bar, GWL_STYLE);
    if (permille < 0) {
        if (!m_marquee) {
            SetWindowLongPtrW(m_bar, GWL_STYLE, style | PBS_MARQUEE);
            SendMessageW(m_bar, PBM_SETMARQUEE, TRUE, 30);
            m_marquee = true;
        }
        return;
    }
    if (m_marquee) {
        SendMessageW(m_bar, PBM_SETMARQUEE, FALSE, 0);
        SetWindowLongPtrW(m_bar, GWL_STYLE, style & ~LONG_PTR(PBS_MARQUEE));
        m_marquee = false;
    }
    SendMessageW(m_bar, PBM_SETPOS, WPARAM(permille), 0);
}

void ProgressWindow::layout(UINT dpi)
{
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)) {
        HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
        if (font) {
            SendMessageW(m_text, WM_SETFONT, WPARAM(font), TRUE);
            if (m_font)
                DeleteObject(m_font);
            m_font = font;
        }
    }
    RECT client{};
    GetClientRect(m_hwnd, &client);
    const int margin = px(16, dpi);
    const int width = client.right - margin * 2;
    MoveWindow(m_text, margin, margin, width, px(40, dpi), TRUE);
    MoveWindow(m_bar, margin, margin + px(50, dpi), width, px(16, dpi), TRUE);
}

LRESULT CALLBACK ProgressWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<ProgressWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CLOSE:
        if (self)
            self->m_closeRequested = true;
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOW));
        SetTextColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOWTEXT));
        return LRESULT(GetSysColorBrush(COLOR_WINDOW));
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (self && self->m_text)
            self->layout(HIWORD(wp));
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace fw::onefile
