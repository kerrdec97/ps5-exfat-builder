#pragma once

// How the app shows up in the Windows shell: its icon, and what a taskbar pin
// starts.
//
// Started through the single-file launcher (src/onefile), the app runs from an
// unpacked copy in a versioned cache folder that a later version removes.
// These window properties make "Pin to taskbar" pin the launcher instead.

#include <QIcon>

class QWidget;

namespace fw::launcher {

// The brand mark (the blue-to-violet tile with the layers glyph) in the sizes
// the title bar, taskbar, Alt-Tab and tray ask for. The same artwork is in
// windows/app.ico, the icon of the exe.
QIcon appIcon();

// No-op unless FW_LAUNCHER_EXE (set by the launcher) names an existing file
// and the window is a real Windows window.
void applyTaskbarIdentity(QWidget* window);

} // namespace fw::launcher
