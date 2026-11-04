#pragma once

// A sheet that browses the console's folders one level at a time and returns
// the chosen folder (legacy _ampr_pick_ps5_folder). Each listing opens its own
// short FTP connection on a pool thread; nothing recursive runs here.

#include "connect/ps5/Endpoint.h"

#include <QString>

class QWidget;

namespace fw::pages {

// Empty when cancelled.
QString pickConsoleFolder(QWidget* host, const ps5::ConsoleEndpoint& endpoint, const QString& start, const QString& title,
                          const QString& actionText);

} // namespace fw::pages
