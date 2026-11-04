#pragma once

// Directory listing parsers for the formats PS5 FTP servers send.
//
//  * LIST, Unix "ls -l" style with owner and group (9+ fields), the legacy
//    parser's only format:  "drwxr-xr-x 1 root root 4096 Jan 01 12:00 name"
//  * LIST, Unix style without a group column (8+ fields), which some
//    homebrew servers print: "-rw-r--r-- 1 root 1234 Jan  1  2024 name"
//  * LIST, DOS/IIS style:   "01-01-24  12:00AM  <DIR>  name"
//  * MLSD facts:            "type=file;size=1234;modify=20240101120000; name"
// Names keep inner spaces exactly; "." and ".." and MLSD cdir/pdir are dropped.

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <optional>
#include <vector>

namespace fw::ps5 {

struct FtpEntry {
    QString name;
    bool isDir = false;
    bool isLink = false;
    qint64 size = -1;      // -1 = unknown
    QDateTime modified;    // UTC when known
    QString linkTarget;
};

// `now` decides the year of "Mon DD HH:MM" timestamps (the last 12 months).
std::optional<FtpEntry> parseListLine(const QString& line, const QDateTime& now = QDateTime::currentDateTimeUtc());
std::optional<FtpEntry> parseMlsdLine(const QString& line);
std::vector<FtpEntry> parseListing(const QByteArray& data, bool mlsd);

// "YYYYMMDDHHMMSS[.sss]" (MLSD modify=, MDTM replies) as UTC.
QDateTime parseFtpTimestamp(const QString& text);

} // namespace fw::ps5
