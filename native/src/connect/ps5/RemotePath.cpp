#include "connect/ps5/RemotePath.h"

namespace fw::ps5 {

namespace {

// Collapses empty and "." segments; ".." removes the previous segment and can
// never climb above "/". No trimming, no backslash conversion.
QString collapse(const QString& p)
{
    QStringList parts;
    for (const QString& seg : p.split(u'/', Qt::SkipEmptyParts)) {
        if (seg == u".")
            continue;
        if (seg == u"..") {
            if (!parts.isEmpty())
                parts.removeLast();
            continue;
        }
        parts << seg;
    }
    return u'/' + parts.join(u'/');
}

} // namespace

QString normalizeRemotePath(const QString& pathIn)
{
    QString p = pathIn.trimmed();
    p.replace(u'\\', u'/');
    return collapse(p);
}

QString cleanRemotePath(const QString& path)
{
    return collapse(path);
}

QString joinRemote(const QString& dir, const QString& name)
{
    // `name` is a trusted relative path ('/'-separated, e.g. from a local
    // folder scan). Its characters are kept exactly: no trimming, no '\\'
    // conversion; "." and ".." segments are dropped so it can never climb.
    QStringList parts;
    for (const QString& seg : name.split(u'/', Qt::SkipEmptyParts))
        if (seg != u"." && seg != u"..")
            parts << seg;
    const QString base = cleanRemotePath(dir);
    if (parts.isEmpty())
        return base;
    return (base == u"/" ? QString() : base) + u'/' + parts.join(u'/');
}

QString remoteNameProblem(const QString& name)
{
    if (name.isEmpty())
        return QStringLiteral("empty name");
    if (name == u"." || name == u"..")
        return QStringLiteral("reserved name \"%1\"").arg(name);
    for (QChar c : name) {
        if (c == u'/' || c == u'\\' || c == u':')
            return QStringLiteral("the name contains \"%1\"").arg(c);
        if (c.unicode() < 0x20)
            return QStringLiteral("the name contains a control character");
    }
    return {};
}

QString localNameProblem(const QString& name)
{
    if (const QString why = remoteNameProblem(name); !why.isEmpty())
        return why;
#ifdef Q_OS_WIN
    for (QChar c : name)
        if (c == u'<' || c == u'>' || c == u'"' || c == u'|' || c == u'?' || c == u'*')
            return QStringLiteral("the name contains \"%1\", which Windows does not allow").arg(c);
    if (name.endsWith(u'.') || name.endsWith(u' '))
        return QStringLiteral("the name ends with a dot or a space, which Windows removes");
    static const QStringList reserved = {QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
                                         QStringLiteral("NUL")};
    const QString stem = name.section(u'.', 0, 0).toUpper();
    if (reserved.contains(stem) || ((stem.startsWith(QLatin1String("COM")) || stem.startsWith(QLatin1String("LPT")))
                                    && stem.size() == 4 && stem.at(3) >= u'1' && stem.at(3) <= u'9'))
        return QStringLiteral("\"%1\" is a reserved device name on Windows").arg(name);
#endif
    return {};
}

std::optional<QString> remoteChild(const QString& dir, const QString& name)
{
    if (!remoteNameProblem(name).isEmpty())
        return std::nullopt;
    const QString base = cleanRemotePath(dir);
    return (base == u"/" ? QString() : base) + u'/' + name;
}

QString remoteParent(const QString& path)
{
    const QString p = cleanRemotePath(path);
    const qsizetype i = p.lastIndexOf(u'/');
    return i <= 0 ? QStringLiteral("/") : p.left(i);
}

QString remoteFileName(const QString& path)
{
    const QString p = cleanRemotePath(path);
    return p.mid(p.lastIndexOf(u'/') + 1);
}

const QStringList& protectedRemoteRoots()
{
    // Firmware, the app registry (/system_data/priv), installed apps, saves and
    // trophies (/user/app, /user/home, /user/system) and the sandbox mounts:
    // deleting or overwriting there needs the extra confirmation.
    static const QStringList roots = {
        QStringLiteral("/system"),       QStringLiteral("/system_ex"),    QStringLiteral("/system_data"),
        QStringLiteral("/system_tmp"),   QStringLiteral("/preinst"),      QStringLiteral("/preinst2"),
        QStringLiteral("/mini-syscore"), QStringLiteral("/sandcastle"),   QStringLiteral("/update"),
        QStringLiteral("/dev"),          QStringLiteral("/proc"),         QStringLiteral("/user/app"),
        QStringLiteral("/user/home"),    QStringLiteral("/user/system"),  QStringLiteral("/mnt/sandbox"),
    };
    return roots;
}

bool isProtectedRemotePath(const QString& path)
{
    if (path.trimmed().isEmpty())
        return false;
    // Normalised first, so "/data/../system" or "//system/" cannot slip through.
    const QString low = normalizeRemotePath(path).toLower();
    for (const QString& prot : protectedRemoteRoots())
        if (low == prot || low.startsWith(prot + u'/'))
            return true;
    return false;
}

QString protectedPathWarning(const QString& path, const QString& action)
{
    return QStringLiteral("The target is a protected PS5 system directory:\n\n    %1\n\nModifying system partitions "
                          "(/system, /system_ex, /preinst, /preinst2 ...) can BRICK your console. This is almost "
                          "never what you want.\n\nContinue with \"%2\" anyway?")
        .arg(path, action);
}

DestinationInfo classifyDestination(const QString& path)
{
    const QString low = path.trimmed().toLower();
    DestinationInfo d;
    if (low.startsWith(u"/mnt/usb") || low.startsWith(u"/mnt/ext") || low.startsWith(u"/mnt/sandisk")
        || low.startsWith(u"/mnt/extusb")) {
        d.kind = DestinationKind::External;
        d.label = QStringLiteral("External USB / exFAT");
        d.note = QStringLiteral("External USB/exFAT is usually the fastest target.");
    } else if (low.startsWith(u"/data") || low.startsWith(u"/mnt/sandbox") || low.startsWith(u"/system")
               || low.startsWith(u"/user")) {
        d.kind = DestinationKind::Internal;
        d.label = QStringLiteral("Internal PS5 storage");
        d.note = QStringLiteral("Internal PS5 storage may be slower on older firmware.");
    } else {
        d.label = QStringLiteral("Unknown location");
        d.note = QStringLiteral("Destination type could not be determined from the path.");
    }
    return d;
}

} // namespace fw::ps5
