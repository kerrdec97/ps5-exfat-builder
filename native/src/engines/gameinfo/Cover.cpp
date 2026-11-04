#include "engines/gameinfo/Cover.h"

#include "core/Paths.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace fw::gameinfo {

namespace {

constexpr qint64 kMaxPixels = 8192LL * 8192LL;

bool allowedFormat(const QByteArray& fmt)
{
    const QByteArray f = fmt.toLower();
    return f == "png" || f == "jpeg" || f == "jpg" || f == "webp" || f == "bmp" || f == "dds";
}

bool nearColour(QRgb a, QRgb b)
{
    constexpr int tol = 12;
    return std::abs(qRed(a) - qRed(b)) <= tol && std::abs(qGreen(a) - qGreen(b)) <= tol
        && std::abs(qBlue(a) - qBlue(b)) <= tol;
}

} // namespace

QString defaultCoverCacheDir()
{
    return QDir(Paths::cacheDir()).filePath(QStringLiteral("covers"));
}

QImage trimCoverBorders(const QImage& input)
{
    QImage img = input;
    // 1. Transparent borders.
    if (img.hasAlphaChannel()) {
        const QImage a = img.convertToFormat(QImage::Format_ARGB32);
        int minX = a.width(), minY = a.height(), maxX = -1, maxY = -1;
        for (int y = 0; y < a.height(); ++y) {
            const QRgb* line = reinterpret_cast<const QRgb*>(a.constScanLine(y));
            for (int x = 0; x < a.width(); ++x)
                if (qAlpha(line[x]) != 0) {
                    minX = std::min(minX, x);
                    maxX = std::max(maxX, x);
                    minY = std::min(minY, y);
                    maxY = std::max(maxY, y);
                }
        }
        if (maxX >= 0 && !(minX == 0 && minY == 0 && maxX == a.width() - 1 && maxY == a.height() - 1))
            img = img.copy(minX, minY, maxX - minX + 1, maxY - minY + 1);
    }

    // 2. Solid-colour borders, bounded (the legacy v2.2.14 heuristic).
    const QImage rgb = img.convertToFormat(QImage::Format_ARGB32);
    const int w = rgb.width(), h = rgb.height();
    if (w < 32 || h < 32)
        return img;
    auto px = [&](int x, int y) { return reinterpret_cast<const QRgb*>(rgb.constScanLine(y))[x]; };
    const QRgb ref = px(0, 0);
    if (!(nearColour(px(w - 1, 0), ref) && nearColour(px(0, h - 1), ref) && nearColour(px(w - 1, h - 1), ref)))
        return img;
    const int stepX = std::max(1, w / 32), stepY = std::max(1, h / 32);
    auto rowMatches = [&](int y) {
        for (int x = 0; x < w; x += stepX)
            if (!nearColour(px(x, y), ref))
                return false;
        return true;
    };
    auto colMatches = [&](int x) {
        for (int y = 0; y < h; y += stepY)
            if (!nearColour(px(x, y), ref))
                return false;
        return true;
    };
    int top = 0, bot = h, left = 0, right = w;
    for (int y = 0; y < h; ++y)
        if (!rowMatches(y)) { top = y; break; }
    for (int y = h - 1; y >= 0; --y)
        if (!rowMatches(y)) { bot = y + 1; break; }
    for (int x = 0; x < w; ++x)
        if (!colMatches(x)) { left = x; break; }
    for (int x = w - 1; x >= 0; --x)
        if (!colMatches(x)) { right = x + 1; break; }
    const double maxTop = h * 0.25, maxLeft = w * 0.25, maxRight = w * 0.75; // legacy bounds, as written
    const int trimBot = h - bot, trimRight = w - right;
    if (top < maxTop && trimBot < maxTop && left < maxLeft && trimRight < maxRight && right > left + 16
        && bot > top + 16 && (top > 0 || bot < h || left > 0 || right < w))
        img = img.copy(left, top, right - left, bot - top);
    return img;
}

Result<QImage> normalizeCoverImage(const QString& sourcePath)
{
    QFile file(sourcePath);
    if (!file.open(QIODevice::ReadOnly))
        return Error(ErrorCode::NotFound, QStringLiteral("The cover image could not be opened."), file.errorString());
    const QByteArray fmt = QImageReader::imageFormat(&file);
    if (!allowedFormat(fmt))
        return Error(ErrorCode::Unsupported, QStringLiteral("The cover is not a PNG, JPEG, WEBP, BMP or DDS image."),
                     QStringLiteral("detected format \"%1\"").arg(QString::fromLatin1(fmt)));
    file.seek(0);
    QImageReader reader(&file, fmt);
    const QSize size = reader.size();
    if (size.isValid() && qint64(size.width()) * size.height() > kMaxPixels)
        return Error(ErrorCode::Unsupported, QStringLiteral("The cover image is too large."),
                     QStringLiteral("%1x%2").arg(size.width()).arg(size.height()));
    QImage img = reader.read();
    if (img.isNull())
        return Error(ErrorCode::CorruptData, QStringLiteral("The cover image could not be decoded."), reader.errorString());
    if (qint64(img.width()) * img.height() > kMaxPixels)
        return Error(ErrorCode::Unsupported, QStringLiteral("The cover image is too large."));

    img = trimCoverBorders(img).convertToFormat(QImage::Format_RGB32);
    const double scale = std::max(double(kCoverWidth) / img.width(), double(kCoverHeight) / img.height());
    const int nw = std::max(1, int(std::lround(img.width() * scale)));
    const int nh = std::max(1, int(std::lround(img.height() * scale)));
    const QImage scaled = img.scaled(nw, nh, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const int x = (nw - kCoverWidth) / 2, y = (nh - kCoverHeight) / 2;
    return scaled.copy(x, y, kCoverWidth, kCoverHeight).convertToFormat(QImage::Format_RGB888);
}

Result<QString> normalizedCover(const QString& sourcePath, const QString& cacheDirIn)
{
    const QFileInfo fi(sourcePath);
    if (!fi.isFile())
        return Error(ErrorCode::NotFound, QStringLiteral("The cover image does not exist."));
    const QString cacheDir = cacheDirIn.isEmpty() ? defaultCoverCacheDir() : cacheDirIn;
    const QString key = QStringLiteral("%1|%2|%3|%4x%5")
                            .arg(QDir::toNativeSeparators(fi.absoluteFilePath()))
                            .arg(fi.lastModified().toSecsSinceEpoch())
                            .arg(fi.size())
                            .arg(kCoverWidth)
                            .arg(kCoverHeight);
    const QString name = QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex());
    const QString out = QDir(cacheDir).filePath(name + QStringLiteral(".png"));
    if (QFileInfo(out).isFile())
        return out;

    auto img = normalizeCoverImage(sourcePath);
    if (!img.ok())
        return img.takeError();
    if (!QDir().mkpath(cacheDir))
        return Error(ErrorCode::IoError, QStringLiteral("The cover cache folder could not be created."),
                     QDir::toNativeSeparators(cacheDir));
    QSaveFile f(out);
    if (!f.open(QIODevice::WriteOnly) || !img.value().save(&f, "PNG") || !f.commit())
        return Error(ErrorCode::IoError, QStringLiteral("The normalised cover could not be saved."), f.errorString());
    return out;
}

} // namespace fw::gameinfo
