#include "engines/backport/GameMetadata.h"

#include <QRegularExpression>
#include <QString>

namespace fw::backport {

std::optional<std::pair<int, int>> parsePs5VersionField(const QVariant& raw)
{
    if (!raw.isValid() || raw.isNull())
        return std::nullopt;
    const QString s = raw.toString().trimmed();
    if (s.isEmpty())
        return std::nullopt;
    if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        QString digits = s.mid(2);
        digits.remove(u'_');
        digits.remove(u'-');
        digits.remove(u' ');
        if (digits.size() < 2)
            return std::nullopt;
        const QString majorStr = digits.left(2);
        const QString minorStr = digits.size() >= 4 ? digits.mid(2, 2) : QStringLiteral("00");
        static const QRegularExpression dec(QStringLiteral("^[0-9]+$"));
        if (!dec.match(majorStr).hasMatch() || !dec.match(minorStr).hasMatch()) {
            bool ok = false;
            const quint64 v = digits.toULongLong(&ok, 16);
            if (!ok)
                return std::nullopt;
            const int nbits = int(digits.size()) * 4;
            const int major = nbits >= 16 ? int((v >> (nbits - 8)) & 0xFF) : int(v & 0xFF);
            return std::make_pair(major, 0);
        }
        return std::make_pair(majorStr.toInt(), minorStr.toInt());
    }
    bool ok = false;
    const int n = s.toInt(&ok, 10);
    if (ok && n >= 0 && n <= 99)
        return std::make_pair(n, 0);
    return std::nullopt;
}

} // namespace fw::backport
