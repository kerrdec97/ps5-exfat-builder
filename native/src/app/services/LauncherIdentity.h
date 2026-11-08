#pragma once

// Started through the single-file launcher (src/onefile), the app runs from an
// unpacked copy in a versioned cache folder that a later version removes.
// These window properties make "Pin to taskbar" pin the launcher instead.

class QWidget;

namespace fw::launcher {

// No-op unless FW_LAUNCHER_EXE (set by the launcher) names an existing file
// and the window is a real Windows window.
void applyTaskbarIdentity(QWidget* window);

} // namespace fw::launcher
