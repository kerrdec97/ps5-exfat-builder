#pragma once

// What the Language row in Settings › General needs besides the setting
// itself (docs/I18N.md): the user's catalogue folder and "Restart now".

#include "core/Result.h"

#include <QString>
#include <QStringList>

#include <functional>

class QWidget;

namespace fw::language {

// Makes the user's catalogue folder (`<data>/lang`, i18n::userDir()) ready to
// be opened: creates it, writes README.txt (how to add or correct a
// language) when it is not there, and keeps `_source.json`, the English
// texts the app shows, a copy of the built-in one. Returns the folder.
Result<QString> prepareUserFolder(const QString& folder);
// The text of README.txt.
QString readmeText();

// The program "Restart now" starts: the single-file exe the app was unpacked
// from (FW_LAUNCHER_EXE), else this exe; with this run's arguments.
struct RestartCommand {
    QString program;
    QStringList arguments;
};
RestartCommand restartCommand();

// Set for the new copy: it waits for this one to let go of the data folder
// instead of saying that the app is already running (main.cpp).
inline constexpr char kRestartEnv[] = "FW_RESTARTED";

// Restarts the app: closes `window` as a normal quit does (it may refuse: an
// unsaved edit, a backport run that is to keep going), then starts
// restartCommand(). False when the window stayed open or the program could
// not be started.
bool restartApp(QWidget* window);

// What "Restart now" calls. Tests swap it; an empty function puts
// restartApp() back.
using RestartFunction = std::function<bool(QWidget* window)>;
void setRestartFunction(RestartFunction function);
bool restart(QWidget* window);

} // namespace fw::language
