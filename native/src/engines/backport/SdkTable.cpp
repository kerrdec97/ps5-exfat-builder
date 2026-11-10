#include "engines/backport/SdkTable.h"

namespace fw::backport {

static std::optional<int> bcdByte(quint32 b)
{
    const quint32 hi = (b >> 4) & 0xF, lo = b & 0xF;
    if (hi > 9 || lo > 9)
        return std::nullopt;
    return int(hi * 10 + lo);
}

std::optional<int> bcdMajor(quint32 value) { return bcdByte((value >> 24) & 0xFF); }
std::optional<int> bcdMinor(quint32 value) { return bcdByte((value >> 16) & 0xFF); }

QString sdkVersionText(quint32 value)
{
    const auto ma = bcdMajor(value), mi = bcdMinor(value);
    if (!ma || !mi)
        return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0'));
    return QStringLiteral("%1.%2").arg(*ma).arg(*mi, 2, 10, QLatin1Char('0'));
}

} // namespace fw::backport
