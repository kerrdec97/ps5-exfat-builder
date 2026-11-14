#include "core/PathSafety.h"

#include "core/FileUtil.h"

#include <QDir>
#include <QSet>
#include <QStringList>

namespace fw {

bool isSafeEntryName(const QString& name)
{
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        return false;
    bool onlyDotsAndSpaces = true;
    for (const QChar ch : name) {
        const char16_t c = ch.unicode();
        if (c < 0x20 || c == u'/' || c == u'\\' || c == u':')
            return false;
        if (c != u'.' && c != u' ')
            onlyDotsAndSpaces = false;
    }
    return !onlyDotsAndSpaces;
}

Result<QString> childPathInside(const QString& root, const QString& relative)
{
    const auto refuse = [&](const QString& why) {
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The path \"%1\" would lead outside the folder %2, so nothing was written there.")
                         .arg(relative, QDir::toNativeSeparators(root)),
                     why);
    };
    if (root.isEmpty())
        return refuse(QStringLiteral("no folder given"));
    for (const QString& part : relative.split(u'/'))
        if (!isSafeEntryName(part))
            return refuse(QStringLiteral("unsafe path component \"%1\"").arg(part));
    const QString base = QDir::cleanPath(root);
    const QString prefix = base.endsWith(u'/') ? base : base + u'/';
    const QString joined = QDir::cleanPath(prefix + relative);
    if (!joined.startsWith(prefix) || joined.size() <= prefix.size())
        return refuse(QStringLiteral("%1 is not inside %2").arg(joined, base));
    return joined;
}

QString hostFileName(const QString& name, bool windowsRules)
{
    if (!windowsRules)
        return name;
    QString out = sanitizeFileName(name);
    // Windows also takes the superscript digits ¹ ² ³ as port numbers, and
    // CONIN$ / CONOUT$ open the console.
    static const QSet<QString> reserved = [] {
        QSet<QString> s = {QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"), QStringLiteral("NUL"),
                           QStringLiteral("CONIN$"), QStringLiteral("CONOUT$")};
        static const QString digits = QStringLiteral("123456789¹²³");
        for (const QChar d : digits) {
            s.insert(QStringLiteral("COM") + d);
            s.insert(QStringLiteral("LPT") + d);
        }
        return s;
    }();
    // The device is found by the name before the first dot, without the
    // spaces in front of that dot ("CON .txt" is CON on Windows 10). None of
    // those names is longer than 7 characters, and upper case never makes a
    // name shorter: a longer one is not looked up (every segment of every
    // name in an archive's list comes here).
    QStringView stem(out);
    if (const qsizetype dot = stem.indexOf(u'.'); dot >= 0)
        stem = stem.first(dot);
    while (stem.endsWith(u' '))
        stem.chop(1);
    if (stem.size() <= 7 && reserved.contains(stem.toString().toUpper()))
        out.prepend(u'_');
    return out;
}

bool hostUsesWindowsNames()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

QString hostEntryName(const QString& name)
{
    const QString local = hostFileName(name, hostUsesWindowsNames());
    return isSafeEntryName(local) ? local : hostFileName(name, true);
}

QString hostNameKey(const QString& name)
{
    if (!hostUsesWindowsNames())
        return name;
    // One UTF-16 unit to one unit, as the NTFS upcase table maps. QString's
    // toUpper() does full case mapping ("ß" becomes "SS"), which would make
    // "straße.dat" and "STRASSE.dat" collide although NTFS keeps both.
    QString key = name;
    for (QChar& ch : key)
        ch = ch.toUpper();
    return key;
}

} // namespace fw
