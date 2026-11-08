#include "engines/ampr/Runtime.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace fw::ampr {

QString bundledRuntimePath()
{
    const QString env = qEnvironmentVariable("FW_AMPR_RUNTIME");
    if (!env.isEmpty())
        return env;
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("engines/ampr-emu-ng/libSceAmpr.sprx"));
}

Result<QByteArray> loadBundledRuntime()
{
    const QString path = bundledRuntimePath();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::DependencyMissing,
                     QStringLiteral("The ampr-emu-ng runtime is missing from this installation (%1).").arg(QDir::toNativeSeparators(path)));
    const QByteArray bytes = f.readAll();
    const QByteArray sha = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    if (sha != QByteArray(kBundledRuntimeSha256))
        return Error(ErrorCode::DependencyMissing,
                     QStringLiteral("The bundled ampr-emu-ng runtime is damaged (its SHA-256 does not match). Reinstall the app."),
                     QString::fromLatin1(sha));
    return bytes;
}

QString bundledRuntimeProblem()
{
    auto r = loadBundledRuntime();
    return r.ok() ? QString() : r.error().message;
}

QString RuntimeInfo::describe() const
{
    switch (kind) {
    case RuntimeKind::None: return QStringLiteral("no libSceAmpr.sprx");
    case RuntimeKind::AmprEmuNg:
        return version.isEmpty() ? QStringLiteral("ampr-emu-ng") : QStringLiteral("ampr-emu-ng %1").arg(version);
    case RuntimeKind::AmprEmu:
        return version.isEmpty() ? QStringLiteral("ampr_emu") : QStringLiteral("ampr_emu %1").arg(version);
    case RuntimeKind::Unknown: break;
    }
    return QStringLiteral("an unknown libSceAmpr.sprx");
}

RuntimeInfo identifyRuntime(const QByteArray& sprx)
{
    RuntimeInfo info;
    if (sprx.isEmpty())
        return info;
    info.bundled = QCryptographicHash::hash(sprx, QCryptographicHash::Sha256).toHex() == QByteArray(kBundledRuntimeSha256);
    static const QRegularExpression ng(QStringLiteral("ampr-emu-ng ([0-9]+\\.[0-9]+\\.[0-9]+)"));
    static const QRegularExpression up(QStringLiteral("([0-9]+\\.[0-9]+\\.[0-9]+(?:\\.[0-9]+)?) \\(c\\) Drakmor"));
    // The strings are ASCII inside the binary; Latin-1 keeps every byte.
    const QString text = QString::fromLatin1(sprx);
    if (const auto m = ng.match(text); m.hasMatch()) {
        info.kind = RuntimeKind::AmprEmuNg;
        info.version = m.captured(1);
    } else if (text.contains(QLatin1String("ampr-emu-ng"))) {
        info.kind = RuntimeKind::AmprEmuNg;
    } else if (text.contains(QLatin1String("[AMPR_EMU]"))) {
        info.kind = RuntimeKind::AmprEmu;
        if (const auto um = up.match(text); um.hasMatch())
            info.version = um.captured(1);
    } else {
        info.kind = RuntimeKind::Unknown;
    }
    if (info.bundled) {
        info.kind = RuntimeKind::AmprEmuNg;
        info.version = QString::fromLatin1(kBundledRuntimeVersion);
    }
    return info;
}

} // namespace fw::ampr
