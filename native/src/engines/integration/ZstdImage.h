#pragma once

// The single-file ".zstd" game (GitHub #98): a bare exFAT volume image named
// "<game>.zstd" whose game files are in ampr-emu-ng zstd packs. Kraken Mount
// MOD (0.2.5 and later) mounts such a file; the "zstd" in the name is the
// packs inside, there is no zstd frame, header or footer around the volume
// (docs/ZSTD_PACKS.md, ".zstd single-file images"). It is exactly what
// image.build.zstd writes for the exFAT format, under another name.
//
// The name alone says nothing about the content, so a .zstd is opened only
// after its first sector was read: the exFAT boot sector ("EXFAT   " at bytes
// 3 to 10, as Kraken Mount MOD checks it). Two refusals name what the file is
// instead: a zstd stream (a whole image compressed into one zstd frame, such
// as PS5ZST's .exfat.zst, which no PS5 loader mounts) and anything else (a
// PFSC container renamed .zstd, a damaged file).

#include "core/Result.h"

#include <QByteArray>
#include <QString>

namespace fw::integration {

// What the first bytes of a .zstd file hold.
enum class ZstdImageKind {
    Exfat,      // an exFAT boot sector: opened as an exFAT image
    ZstdStream, // a zstd frame (28 B5 2F FD) or a skippable frame (5x 2A 4D 18)
    Other,      // anything else, a file shorter than one sector included
};

// True for a file named *.zstd (any case).
bool isZstdImageName(const QString& path);

// The kind of the first bytes of a file (at least the first 11 are looked at).
ZstdImageKind sniffZstdImage(const QByteArray& head);

// Reads the first sector of `path` and tells its kind. IoError when the file
// cannot be read.
Result<ZstdImageKind> sniffZstdImageFile(const QString& path);

// Success when `path` holds an exFAT image. Otherwise Unsupported with the
// text the user reads (the file named), or the read error.
Status checkZstdImage(const QString& path);

// "exfat", "ffpkg", "ffpfs" or "ffpfsc" for an image file by its name, with a
// .zstd counted as "exfat" (checkZstdImage() says whether it really is one);
// "" for anything else.
QString imageFormatByName(const QString& path);

} // namespace fw::integration
