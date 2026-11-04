#include "engines/gameinfo/Version.h"

#include <QStringList>

#include <cmath>

namespace fw::gameinfo {

namespace {

bool isBcdByte(quint32 b)
{
    return (b >> 4) <= 9 && (b & 0xF) <= 9;
}

int bcd(quint32 b)
{
    return int((b >> 4) * 10 + (b & 0xF));
}

SystemVersion fromBytes(quint32 majorByte, quint32 minorByte)
{
    SystemVersion v;
    if (isBcdByte(majorByte) && isBcdByte(minorByte)) {
        const int mj = bcd(majorByte);
        if (mj >= 1 && mj <= 99) {
            v.majorVersion = mj;
            v.minorVersion = bcd(minorByte);
        }
        return v;
    }
    // Not BCD: the legacy app read the byte as a plain number.
    if (majorByte >= 1 && majorByte <= 30) {
        v.majorVersion = int(majorByte);
        v.minorVersion = isBcdByte(minorByte) ? bcd(minorByte) : 0;
    }
    return v;
}

bool allDecimal(const QString& s)
{
    if (s.isEmpty())
        return false;
    for (QChar c : s)
        if (c < u'0' || c > u'9')
            return false;
    return true;
}

} // namespace

QString SystemVersion::toString() const
{
    if (!isValid())
        return {};
    return QStringLiteral("%1.%2").arg(majorVersion).arg(qMax(0, minorVersion), 2, 10, QLatin1Char('0'));
}

QString SystemVersion::majorLabel() const
{
    return isValid() ? QStringLiteral("%1.xx").arg(majorVersion) : QString();
}

SystemVersion decodeSystemVersion(quint32 packed)
{
    const quint32 b3 = (packed >> 24) & 0xFF;
    const quint32 b2 = (packed >> 16) & 0xFF;
    const quint32 b1 = (packed >> 8) & 0xFF;
    if (b3 != 0)
        return fromBytes(b3, b2);
    if (b2 != 0)
        return fromBytes(b2, b1);
    return {};
}

SystemVersion parseVersionField(const QJsonValue& value)
{
    SystemVersion v;
    QString s;
    if (value.isDouble()) {
        const double d = value.toDouble();
        if (!std::isfinite(d) || d < 1)
            return v;
        if (d >= 1 && d <= 99 && std::floor(d) == d) {
            v.majorVersion = int(d);
            v.minorVersion = 0;
            return v;
        }
        // A packed quad word written as a number.
        s = QStringLiteral("0x%1").arg(quint64(d), 16, 16, QLatin1Char('0'));
    } else if (value.isString()) {
        s = value.toString().trimmed();
    } else {
        return v;
    }
    if (s.isEmpty())
        return v;
    if (s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        QString digits = s.mid(2);
        digits.remove(u'_').remove(u'-').remove(u' ');
        if (digits.size() < 2)
            return v;
        const QString majorStr = digits.left(2);
        const QString minorStr = digits.size() >= 4 ? digits.mid(2, 2) : QStringLiteral("00");
        if (allDecimal(majorStr) && allDecimal(minorStr)) {
            v.majorVersion = majorStr.toInt();
            v.minorVersion = minorStr.toInt();
            if (v.majorVersion == 0)
                v = {};
            return v;
        }
        bool ok = false;
        const quint64 n = digits.toULongLong(&ok, 16);
        if (!ok || digits.size() > 16)
            return v;
        const int nbits = int(digits.size()) * 4;
        const quint64 mj = nbits >= 16 ? (n >> (nbits - 8)) & 0xFF : n & 0xFF;
        if (mj >= 1 && mj <= 30) {
            v.majorVersion = int(mj);
            v.minorVersion = 0;
        }
        return v;
    }
    bool ok = false;
    const int n = s.toInt(&ok, 10);
    if (ok && n >= 1 && n <= 99) {
        v.majorVersion = n;
        v.minorVersion = 0;
    }
    return v;
}

QString normalizeContentVersion(const QString& version)
{
    const QString v = version.trimmed();
    if (v.isEmpty())
        return v;
    QStringList parts = v.split(u'.');
    if (parts.size() == 2)
        parts << QStringLiteral("000");
    if (parts.size() >= 3) {
        parts[0] = parts[0].rightJustified(2, u'0');
        parts[1] = parts[1].rightJustified(3, u'0');
        parts[2] = parts[2].rightJustified(3, u'0');
        return parts.mid(0, 3).join(u'.');
    }
    return v;
}

} // namespace fw::gameinfo
