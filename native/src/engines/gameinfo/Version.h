#pragma once

// Version fields found in PS4/PS5 metadata.
//
//  * System software / SDK versions are packed BCD: the hex digits are the
//    printed digits. param.sfo SYSTEM_VER 0x04508001 is "4.50";
//    param.json "0x1270000000000000" is "12.70".
//  * Content versions are "XX.XXX.XXX"; the legacy app padded shorter forms
//    ("01.00" -> "01.000.000").

#include <QJsonValue>
#include <QString>

namespace fw::gameinfo {

struct SystemVersion {
    int majorVersion = -1;
    int minorVersion = -1;

    bool isValid() const { return majorVersion >= 1; }
    // "4.50", "10.00"; empty when invalid.
    QString toString() const;
    // "4.xx" — how the legacy Library showed SDKs.
    QString majorLabel() const;
    bool operator==(const SystemVersion& o) const { return majorVersion == o.majorVersion && minorVersion == o.minorVersion; }
};

// Decodes a 32-bit packed system version (param.sfo SYSTEM_VER, the top 32
// bits of a param.json quad word). The top byte is the major, the next the
// minor, both BCD. When the top byte is 0 the next byte is tried as the major
// (the legacy "shift 24, then 16" rule). Majors outside 1..99 (BCD) or 1..30
// (non-BCD fallback, the legacy plausibility range) are rejected.
SystemVersion decodeSystemVersion(quint32 packed);

// Parses a param.json version field like the legacy _parse_ps5_version_field:
// "0x1270000000000000" -> 12.70 (hex characters read as digits); non-BCD hex
// falls back to the numeric top byte; a plain decimal 0..99 is a major. JSON
// numbers above 99 are read as the 64-bit packed value.
SystemVersion parseVersionField(const QJsonValue& value);

// Legacy content-version normalisation: 2 parts get ".000"; with 3+ parts the
// first three are zero-padded to 2/3/3 digits; anything else is unchanged.
QString normalizeContentVersion(const QString& version);

} // namespace fw::gameinfo
