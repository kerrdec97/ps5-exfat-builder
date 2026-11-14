#include "engines/ampr/Runtime.h"

#include "core/Tr.h"

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

namespace {

enum class Problem { None, Missing, Damaged };

struct Loaded {
    Problem problem = Problem::None;
    QString path;
    QByteArray bytes;
    QByteArray sha; // hex
};

Loaded load()
{
    Loaded l;
    l.path = bundledRuntimePath();
    QFile f(l.path);
    if (!f.open(QIODevice::ReadOnly)) {
        l.problem = Problem::Missing;
        return l;
    }
    l.bytes = f.readAll();
    l.sha = QCryptographicHash::hash(l.bytes, QCryptographicHash::Sha256).toHex();
    if (l.sha != QByteArray(kBundledRuntimeSha256))
        l.problem = Problem::Damaged;
    return l;
}

} // namespace

// The errors are English, as every engine's (they reach the log, the job
// records and problem reports); the list of tools shows the same texts in the
// interface's language (bundledRuntimeProblemShown()).
Result<QByteArray> loadBundledRuntime()
{
    Loaded l = load();
    switch (l.problem) {
    case Problem::Missing:
        return Error(ErrorCode::DependencyMissing, QStringLiteral("The ampr-emu-ng runtime is missing from this installation (%1).")
                                                       .arg(QDir::toNativeSeparators(l.path)));
    case Problem::Damaged:
        return Error(ErrorCode::DependencyMissing,
                     QStringLiteral("The bundled ampr-emu-ng runtime is damaged (its SHA-256 does not match). Reinstall the app."),
                     QString::fromLatin1(l.sha));
    case Problem::None: break;
    }
    return std::move(l.bytes);
}

QString bundledRuntimeProblem()
{
    auto r = loadBundledRuntime();
    return r.ok() ? QString() : r.error().message;
}

QString bundledRuntimeProblemShown()
{
    const Loaded l = load();
    switch (l.problem) {
    case Problem::Missing:
        return FW_TR("The ampr-emu-ng runtime is missing from this installation (%1).").arg(QDir::toNativeSeparators(l.path));
    case Problem::Damaged:
        return FW_TR("The bundled ampr-emu-ng runtime is damaged (its SHA-256 does not match). Reinstall the app.");
    case Problem::None: break;
    }
    return {};
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
