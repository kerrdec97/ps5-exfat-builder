#include "connect/ps5/RemotePath.h"

namespace fw::ps5 {

QString normalizeRemotePath(const QString& pathIn)
{
    QString p = pathIn.trimmed();
    p.replace(u'\\', u'/');
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

QString joinRemote(const QString& dir, const QString& name)
{
    return normalizeRemotePath(dir + u'/' + name);
}

QString remoteParent(const QString& path)
{
    const QString p = normalizeRemotePath(path);
    const qsizetype i = p.lastIndexOf(u'/');
    return i <= 0 ? QStringLiteral("/") : p.left(i);
}

QString remoteFileName(const QString& path)
{
    const QString p = normalizeRemotePath(path);
    return p.mid(p.lastIndexOf(u'/') + 1);
}

const QStringList& protectedRemoteRoots()
{
    static const QStringList roots = {
        QStringLiteral("/system"),       QStringLiteral("/system_ex"),  QStringLiteral("/preinst"),
        QStringLiteral("/preinst2"),     QStringLiteral("/mini-syscore"), QStringLiteral("/sandcastle"),
        QStringLiteral("/update"),       QStringLiteral("/dev"),        QStringLiteral("/proc"),
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
