#pragma once

// Cover art normalisation (legacy _normalize_cover / _trim_cover_borders).
//
// source -> border trim (transparent, then a uniform colour within 12 levels,
// each side bounded) -> scale to FILL 260x390 -> centre crop -> PNG.
// Only PNG, JPEG, WEBP, BMP and DDS are decoded (whatever of those the Qt
// image plugins provide), and images over 8192x8192 pixels are refused, so a
// crafted file in a dump cannot reach other decoders or exhaust memory.
// Results are cached by sha1("<abs path>|<mtime s>|<size>|260x390").

#include "core/Result.h"

#include <QImage>
#include <QString>

namespace fw::gameinfo {

constexpr int kCoverWidth = 260;
constexpr int kCoverHeight = 390;

// Default cache folder: Paths::cacheDir()/covers.
QString defaultCoverCacheDir();

// Path of the normalised PNG for `sourcePath`, creating it when the cache has
// none. `cacheDir` empty = defaultCoverCacheDir().
Result<QString> normalizedCover(const QString& sourcePath, const QString& cacheDir = {});

// The pipeline without the cache (tests, previews).
Result<QImage> normalizeCoverImage(const QString& sourcePath);
QImage trimCoverBorders(const QImage& image);

} // namespace fw::gameinfo
