#pragma once

// param.sfo (PSF) reader for PS4 and PS5 dumps.
//
// Layout (little endian): header magic 0x46535000 ("\0PSF"), version,
// key table offset, data table offset, entry count; then 16-byte entries
// {u16 key offset, u16 format, u32 length, u32 max length, u32 data offset}.
// Formats: 0x0204 UTF-8 string (NUL terminated), 0x0004 UTF-8 "special"
// (not terminated), 0x0404 u32. Same rules as the legacy parse_sfo().

#include "core/Result.h"

#include <QByteArray>
#include <QString>

#include <optional>
#include <vector>

namespace fw::gameinfo {

struct SfoEntry {
    QString key;
    quint16 format = 0;
    quint32 length = 0;
    quint32 maxLength = 0;
    QByteArray raw; // `length` bytes of the value

    bool isString() const { return format == 0x0204 || format == 0x0004; }
    bool isInteger() const { return format == 0x0404; }
    QString text() const;      // UTF-8 (invalid bytes replaced), trailing NULs removed
    quint32 integer() const;   // 0 unless isInteger()
};

struct SfoFile {
    quint32 version = 0;
    std::vector<SfoEntry> entries;
    int skippedEntries = 0; // entries whose key or value lay outside the file

    const SfoEntry* find(const QString& key) const;
    // String value of `key`; empty when missing or not a string.
    QString string(const QString& key) const;
    std::optional<quint32> integer(const QString& key) const;
};

constexpr quint32 kSfoMagic = 0x46535000;

Result<SfoFile> parseSfo(const QByteArray& data);
Result<SfoFile> readSfo(const QString& path);

} // namespace fw::gameinfo
