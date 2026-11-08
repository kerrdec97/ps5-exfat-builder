#include "connect/ps5/FtpListing.h"

#include <QRegularExpression>
#include <QStringList>
#include <QTimeZone>

namespace fw::ps5 {

namespace {

int monthFromName(const QString& m)
{
    static const QStringList months = {QStringLiteral("jan"), QStringLiteral("feb"), QStringLiteral("mar"),
                                       QStringLiteral("apr"), QStringLiteral("may"), QStringLiteral("jun"),
                                       QStringLiteral("jul"), QStringLiteral("aug"), QStringLiteral("sep"),
                                       QStringLiteral("oct"), QStringLiteral("nov"), QStringLiteral("dec")};
    return int(months.indexOf(m.left(3).toLower())) + 1;
}

bool skipName(const QString& n)
{
    return n.isEmpty() || n == u"." || n == u"..";
}

} // namespace

QDateTime parseFtpTimestamp(const QString& textIn)
{
    const QString text = textIn.trimmed();
    if (text.size() < 14)
        return {};
    const QDateTime dt = QDateTime::fromString(text.left(14), QStringLiteral("yyyyMMddHHmmss"));
    if (!dt.isValid())
        return {};
    QDateTime utc(dt.date(), dt.time(), QTimeZone::utc());
    return utc;
}

std::optional<FtpEntry> parseListLine(const QString& lineIn, const QDateTime& now)
{
    QString line = lineIn;
    while (line.endsWith(u'\r') || line.endsWith(u'\n'))
        line.chop(1);
    if (line.trimmed().isEmpty() || line.startsWith(QLatin1String("total "), Qt::CaseInsensitive))
        return std::nullopt;

    // Unix style. The group column is optional; backtracking settles it.
    static const QRegularExpression unixRe(QStringLiteral(
        "^([\\-dlcbpsD])[rwxsStTl\\-]{9}[@+.]?\\s+\\d+\\s+\\S+\\s+(?:\\S+\\s+)?(\\d+)\\s+"
        "([A-Za-z]{3})\\s+(\\d{1,2})\\s+(\\d{1,2}:\\d{2}(?::\\d{2})?|\\d{4})\\s+(.*)$"));
    if (const auto m = unixRe.match(line); m.hasMatch()) {
        FtpEntry e;
        const QChar type = m.captured(1).front();
        e.isDir = type == u'd' || type == u'D';
        e.isLink = type == u'l';
        e.size = m.captured(2).toLongLong();
        QString name = m.captured(6);
        if (e.isLink) {
            const qsizetype arrow = name.indexOf(QLatin1String(" -> "));
            if (arrow >= 0) {
                e.linkTarget = name.mid(arrow + 4);
                name = name.left(arrow);
            }
        }
        e.name = name;
        if (skipName(e.name))
            return std::nullopt;
        const int month = monthFromName(m.captured(3));
        const int day = m.captured(4).toInt();
        const QString tod = m.captured(5);
        if (month > 0) {
            if (tod.contains(u':')) {
                const QTime t = QTime::fromString(tod.left(5), QStringLiteral("H:mm"));
                int year = now.date().year();
                QDateTime dt(QDate(year, month, day), t, QTimeZone::utc());
                if (dt.isValid() && dt > now.addDays(2))
                    dt = QDateTime(QDate(year - 1, month, day), t, QTimeZone::utc());
                e.modified = dt;
            } else {
                e.modified = QDateTime(QDate(tod.toInt(), month, day), QTime(0, 0), QTimeZone::utc());
            }
        }
        return e;
    }

    // DOS / IIS style.
    static const QRegularExpression dosRe(QStringLiteral(
        "^(\\d{2})-(\\d{2})-(\\d{2,4})\\s+(\\d{1,2}):(\\d{2})\\s*([AaPp][Mm])?\\s+(<DIR>|\\d+)\\s+(.*)$"));
    if (const auto m = dosRe.match(line); m.hasMatch()) {
        FtpEntry e;
        e.isDir = m.captured(7).compare(QLatin1String("<DIR>"), Qt::CaseInsensitive) == 0;
        e.size = e.isDir ? 0 : m.captured(7).toLongLong();
        e.name = m.captured(8);
        if (skipName(e.name))
            return std::nullopt;
        int year = m.captured(3).toInt();
        if (year < 100)
            year += year < 70 ? 2000 : 1900;
        int hour = m.captured(4).toInt();
        const QString ampm = m.captured(6).toLower();
        if (ampm == u"pm" && hour < 12)
            hour += 12;
        if (ampm == u"am" && hour == 12)
            hour = 0;
        e.modified = QDateTime(QDate(year, m.captured(1).toInt(), m.captured(2).toInt()),
                               QTime(hour, m.captured(5).toInt()), QTimeZone::utc());
        return e;
    }
    return std::nullopt;
}

std::optional<FtpEntry> parseMlsdLine(const QString& lineIn)
{
    QString line = lineIn;
    while (line.endsWith(u'\r') || line.endsWith(u'\n'))
        line.chop(1);
    const qsizetype sp = line.indexOf(u' ');
    if (sp < 0)
        return std::nullopt;
    FtpEntry e;
    e.name = line.mid(sp + 1);
    QString type;
    for (const QString& fact : line.left(sp).split(u';', Qt::SkipEmptyParts)) {
        const qsizetype eq = fact.indexOf(u'=');
        if (eq < 0)
            continue;
        const QString k = fact.left(eq).toLower();
        const QString v = fact.mid(eq + 1);
        if (k == u"type")
            type = v.toLower();
        else if (k == u"size" || k == u"sizd") {
            bool ok = false;
            const qint64 n = v.toLongLong(&ok);
            if (ok && (k == u"size" || e.size < 0))
                e.size = n;
        } else if (k == u"modify")
            e.modified = parseFtpTimestamp(v);
    }
    if (type == u"cdir" || type == u"pdir" || skipName(e.name))
        return std::nullopt;
    e.isDir = type == u"dir";
    e.isLink = type.startsWith(QLatin1String("os.unix=slink")) || type.startsWith(QLatin1String("os.unix=symlink"));
    // No size fact (it is optional): -1 = unknown, so a download asks SIZE
    // instead of expecting 0 bytes.
    return e;
}

std::vector<FtpEntry> parseListing(const QByteArray& data, bool mlsd)
{
    std::vector<FtpEntry> out;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const QByteArray& raw : data.split('\n')) {
        const QString line = QString::fromUtf8(raw);
        auto e = mlsd ? parseMlsdLine(line) : parseListLine(line, now);
        if (e)
            out.push_back(std::move(*e));
    }
    return out;
}

} // namespace fw::ps5
