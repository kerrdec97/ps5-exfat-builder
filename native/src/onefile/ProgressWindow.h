#pragma once

// The small window the launcher shows while it unpacks for longer than a
// second: one line of text and a progress bar. Closing it cancels.

#include <windows.h>

#include <string>

namespace fw::onefile {

class ProgressWindow {
public:
    ~ProgressWindow() { destroy(); }
    bool create(HINSTANCE instance, const std::wstring& title);
    void destroy();
    bool isOpen() const { return m_hwnd != nullptr; }
    HWND hwnd() const { return m_hwnd; }
    bool closeRequested() const { return m_closeRequested; }
    void setText(const std::wstring& text);
    // permille < 0: busy (marquee) bar.
    void setProgress(int permille);

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    void layout(UINT dpi);

    HWND m_hwnd = nullptr;
    HWND m_text = nullptr;
    HWND m_bar = nullptr;
    HFONT m_font = nullptr;
    bool m_closeRequested = false;
    bool m_marquee = false;
    std::wstring m_lastText;
    int m_lastPermille = -2;
};

} // namespace fw::onefile
