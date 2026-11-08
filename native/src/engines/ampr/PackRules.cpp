#include "engines/ampr/PackRules.h"

#include "engines/ampr/ZstdPack.h"

#include <QByteArray>
#include <QSet>

namespace fw::ampr {

namespace {

QString lower(const QString& s) { return s.toLower(); }

QString suffixOf(const QString& name)
{
    const qsizetype dot = name.lastIndexOf(u'.');
    return dot <= 0 ? QString() : lower(name.mid(dot + 1));
}

const QSet<QString>& keepLooseSuffixes()
{
    // ps5-dump-forge compatibility strategy (docs/LZ4.md): container indexes,
    // configs and text, Unreal project files, images, video and audio.
    static const QSet<QString> s = {
        // containers and their indexes
        QStringLiteral("pak"), QStringLiteral("utoc"), QStringLiteral("ucas"),
        // configs and text
        QStringLiteral("ini"), QStringLiteral("json"), QStringLiteral("txt"), QStringLiteral("cfg"), QStringLiteral("xml"),
        QStringLiteral("yaml"), QStringLiteral("yml"), QStringLiteral("csv"), QStringLiteral("lua"), QStringLiteral("dat"),
        // Unreal project files
        QStringLiteral("uproject"), QStringLiteral("uplugin"), QStringLiteral("upluginmanifest"),
        // images
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("dds"), QStringLiteral("bmp"),
        QStringLiteral("tga"),
        // video
        QStringLiteral("bik"), QStringLiteral("bk2"), QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mov"),
        QStringLiteral("webm"), QStringLiteral("usm"), QStringLiteral("mpg"), QStringLiteral("mpeg"), QStringLiteral("m2v"),
        QStringLiteral("avi"), QStringLiteral("wmv"),
        // audio
        QStringLiteral("bnk"), QStringLiteral("wem"), QStringLiteral("wav"), QStringLiteral("ogg"), QStringLiteral("mp3"),
        QStringLiteral("at9"), QStringLiteral("fsb"), QStringLiteral("bank"), QStringLiteral("awb"), QStringLiteral("acb"),
        QStringLiteral("xwm"), QStringLiteral("opus"), QStringLiteral("flac"),
    };
    return s;
}

const QSet<QString>& protectedFolders()
{
    static const QSet<QString> s = {QStringLiteral("sce_module"), QStringLiteral("system"), QStringLiteral("save"),
                                    QStringLiteral("saves"), QStringLiteral("savedata"), QStringLiteral("sce_sys")};
    return s;
}

const QSet<QString>& videoFolders()
{
    static const QSet<QString> s = {QStringLiteral("video"),    QStringLiteral("videos"),    QStringLiteral("movie"),
                                    QStringLiteral("movies"),   QStringLiteral("cutscene"),  QStringLiteral("cutscenes"),
                                    QStringLiteral("fmv")};
    return s;
}

bool insomniacContainer(const QString& name)
{
    // Extensionless boot and streaming containers, by file name.
    const QString n = lower(name);
    if (n.startsWith(QLatin1String("bootload_")) || n == u"critbootload")
        return true;
    for (const char* base : {"soundbank", "wem", "movie", "movies"}) {
        const QString b = QLatin1String(base);
        if (n == b || n.startsWith(b + u'.') || n.startsWith(b + u'_'))
            return true;
    }
    return n.startsWith(QLatin1String("screenreaderwem"));
}

} // namespace

PackStrategy packStrategyFromName(const QString& name)
{
    return name.compare(QLatin1String("compatible"), Qt::CaseInsensitive) == 0 ? PackStrategy::Compatible : PackStrategy::Everything;
}

QString packStrategyName(PackStrategy s)
{
    return s == PackStrategy::Compatible ? QStringLiteral("compatible") : QStringLiteral("everything");
}

QString alwaysLooseReason(const QString& relPath, qint64 size)
{
    if (size == 0)
        return QStringLiteral("empty file");
    const QStringList parts = relPath.split(u'/');
    const QString name = parts.last();
    const QString lname = lower(name);
    if (lname == u"eboot.bin")
        return QStringLiteral("executable");
    const QString suf = suffixOf(name);
    if (suf == u"elf" || suf == u"self" || suf == u"prx" || suf == u"sprx")
        return QStringLiteral("executable or module");
    if (parts.size() > 1) {
        const QString root = lower(parts.first());
        if (root == u"sce_sys" || root == u"sce_module")
            return QStringLiteral("system folder");
        if (root.startsWith(QLatin1String("fakelib")))
            return QStringLiteral("fakelib folder");
    } else {
        static const QSet<QString> ampr = {QStringLiteral("ampr_emu.index"), QStringLiteral("ampr_assets.index"),
                                           QStringLiteral("ampr_assets.index.crc"), QStringLiteral("ampr_assets.index.runtime"),
                                           QStringLiteral("ampr_commands.bin"), QStringLiteral("ampr_emu.log"),
                                           QStringLiteral("apr_emu.log")};
        if (ampr.contains(lname))
            return QStringLiteral("AMPR runtime file");
        if (isZstdPackFileName(name))
            return QStringLiteral("zstd pack file");
    }
    return {};
}

QString compatibleLooseReason(const QString& relPath)
{
    const QStringList parts = relPath.split(u'/');
    if (parts.size() == 1)
        return QStringLiteral("root file");
    for (qsizetype i = 0; i + 1 < parts.size(); ++i) {
        const QString d = lower(parts[i]);
        if (protectedFolders().contains(d) || d.startsWith(QLatin1String("fakelib")))
            return QStringLiteral("protected folder");
        if (videoFolders().contains(d))
            return QStringLiteral("video folder");
    }
    const QString name = parts.last();
    if (keepLooseSuffixes().contains(suffixOf(name)))
        return QStringLiteral("keep-loose type (.%1)").arg(suffixOf(name));
    if (insomniacContainer(name))
        return QStringLiteral("boot or streaming container");
    return {};
}

QString looseReason(const QString& relPath, qint64 size, PackStrategy strategy)
{
    QString r = alwaysLooseReason(relPath, size);
    if (r.isEmpty() && strategy == PackStrategy::Compatible)
        r = compatibleLooseReason(relPath);
    return r;
}

bool packJobDrops(const QString& relPath)
{
    if (relPath.contains(u'/'))
        return false;
    const QString l = lower(relPath);
    return l == u"ampr_commands.bin" || l == u"ampr_emu.log" || l == u"apr_emu.log" || isZstdPackFileName(relPath);
}

ExecutableKind executableKind(const QByteArray& head)
{
    if (head.size() < 4)
        return ExecutableKind::Unknown;
    const QByteArray m = head.left(4);
    if (m == QByteArray("\x54\x14\xF5\xEE", 4) || m == QByteArray("\x4F\x15\x3D\x1D", 4))
        return ExecutableKind::Self;
    if (m == QByteArray("\x7F" "ELF", 4))
        return ExecutableKind::PlainElf;
    return ExecutableKind::Unknown;
}

QStringList knownPackProblemTitles()
{
    return {QStringLiteral("Crimson Desert"),
            QStringLiteral("God of War Ragnarök"),
            QStringLiteral("Sonic X Shadow Generations"),
            QStringLiteral("Forza Horizon 5"),
            QStringLiteral("Kingdom Come: Deliverance II"),
            QStringLiteral("Metal Gear Solid 4"),
            QStringLiteral("Ratchet & Clank: Rift Apart"),
            QStringLiteral("Marvel's Wolverine"),
            QStringLiteral("7 Days to Die"),
            QStringLiteral("Astro Bot")};
}

} // namespace fw::ampr
