#pragma once

// Lucide line icons (ISC licence), rendered from SVG at the exact device pixel
// ratio and tinted to any colour, cached. One icon per concept — names follow
// the design system's icon table (home, build, extract, settings, ...).

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QStringList>

namespace fw::ui {

// A pixmap of `name` at `size` logical pixels, tinted `color`, for `dpr`.
QPixmap iconPixmap(const QString& name, int size, const QColor& color, qreal dpr = 1.0);

// A QIcon with Normal/Active/Selected/Disabled variants coloured for the
// design system (rest, hover, selected, disabled).
QIcon icon(const QString& name, const QColor& rest, const QColor& active = {}, const QColor& selected = {},
           const QColor& disabled = {});

// Registers the icon resources (static library); safe to call repeatedly.
void initIcons();
bool hasIcon(const QString& name);
QStringList iconNames();

} // namespace fw::ui
