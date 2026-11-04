#include "app/ui/Icons.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QMutex>
#include <QPainter>
#include <QSvgRenderer>

#include <cmath>

namespace fw::ui {

namespace {

QByteArray svgSource(const QString& name)
{
    static QHash<QString, QByteArray> cache;
    static QMutex mutex;
    QMutexLocker lock(&mutex);
    auto it = cache.find(name);
    if (it != cache.end())
        return *it;
    QFile f(QStringLiteral(":/icons/%1.svg").arg(name));
    QByteArray data;
    if (f.open(QIODevice::ReadOnly))
        data = f.readAll();
    cache.insert(name, data);
    return data;
}

QPixmap render(const QString& name, int size, const QColor& color, qreal dpr)
{
    QByteArray svg = svgSource(name);
    if (svg.isEmpty())
        svg = svgSource(QStringLiteral("circle_dashed"));
    svg.replace("currentColor", color.name(QColor::HexRgb).toLatin1());
    QSvgRenderer renderer(svg);
    const int px = int(std::ceil(size * dpr));
    QPixmap pm(px, px);
    pm.fill(Qt::transparent);
    {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        if (color.alpha() < 255)
            p.setOpacity(color.alphaF());
        renderer.render(&p, QRectF(0, 0, px, px));
    }
    pm.setDevicePixelRatio(dpr);
    return pm;
}

// Renders crisply at whatever size/dpr Qt asks for.
class TintedIconEngine : public QIconEngine {
public:
    TintedIconEngine(QString name, QColor rest, QColor active, QColor selected, QColor disabled)
        : m_name(std::move(name)), m_rest(rest), m_active(active.isValid() ? active : rest),
          m_selected(selected.isValid() ? selected : m_active),
          m_disabled(disabled.isValid() ? disabled : QColor(rest.red(), rest.green(), rest.blue(), 110))
    {
    }

    QColor colorFor(QIcon::Mode mode) const
    {
        switch (mode) {
        case QIcon::Disabled: return m_disabled;
        case QIcon::Active: return m_active;
        case QIcon::Selected: return m_selected;
        case QIcon::Normal: break;
        }
        return m_rest;
    }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) override
    {
        const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        const int s = std::min(rect.width(), rect.height());
        const QPixmap pm = iconPixmap(m_name, s, colorFor(mode), dpr);
        painter->drawPixmap(QRect(rect.x() + (rect.width() - s) / 2, rect.y() + (rect.height() - s) / 2, s, s), pm);
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State, qreal scale) override
    {
        return iconPixmap(m_name, std::min(size.width(), size.height()), colorFor(mode), scale);
    }

    QIconEngine* clone() const override { return new TintedIconEngine(*this); }
    QString key() const override { return QStringLiteral("fw-tinted"); }

private:
    QString m_name;
    QColor m_rest, m_active, m_selected, m_disabled;
};

} // namespace

QPixmap iconPixmap(const QString& name, int size, const QColor& color, qreal dpr)
{
    static QHash<QString, QPixmap> cache;
    static QMutex mutex;
    const QString key = QStringLiteral("%1|%2|%3|%4").arg(name).arg(size).arg(color.name(QColor::HexArgb)).arg(dpr);
    {
        QMutexLocker lock(&mutex);
        auto it = cache.find(key);
        if (it != cache.end())
            return *it;
    }
    QPixmap pm = render(name, size, color, dpr);
    QMutexLocker lock(&mutex);
    if (cache.size() > 4000)
        cache.clear();
    cache.insert(key, pm);
    return pm;
}

QIcon icon(const QString& name, const QColor& rest, const QColor& active, const QColor& selected, const QColor& disabled)
{
    return QIcon(new TintedIconEngine(name, rest, active, selected, disabled));
}

bool hasIcon(const QString& name)
{
    return QFile::exists(QStringLiteral(":/icons/%1.svg").arg(name));
}

QStringList iconNames()
{
    QStringList out;
    for (const QString& f : QDir(QStringLiteral(":/icons")).entryList({QStringLiteral("*.svg")}))
        out << f.chopped(4);
    return out;
}

} // namespace fw::ui
