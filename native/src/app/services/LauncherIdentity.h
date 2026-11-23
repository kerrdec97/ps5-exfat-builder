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

// True when the process `pid` owns a visible top-level window. A copy of the
// app that holds the instance lock without one is still closing (it waits
// for a job to stop). Off Windows: true (nothing to tell apart).
bool processHasVisibleWindow(qint64 pid);

// Windows log-off or shut down while the app is at work: the text Windows
// shows beside the app in its "These apps are preventing shutdown" list
// (empty: none any more). Off Windows, or for a window without a native
// handle: nothing.
void setShutdownBlockReason(QWidget* window, const QString& reason);

} // namespace fw::launcher
