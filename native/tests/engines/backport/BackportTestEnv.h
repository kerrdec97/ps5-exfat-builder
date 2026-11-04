#pragma once

// Synthetic game trees (docs/BACKPORT_PARITY.md §6.5 G1) and a run
// environment for the backport tests: a game folder, ZIP folders, a fakelib
// set and a job context, all inside one QTemporaryDir.

#include "BackportFixtures.h"

#include "core/Cancellation.h"
#include "core/FileUtil.h"
#include "core/Log.h"
#include "core/Progress.h"
#include "engines/backport/BackportOptions.h"
#include "engines/backport/BackportRunner.h"
#include "jobs/Job.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QMap>
#include <QTemporaryDir>

#include <memory>

namespace fwtest::bp {

enum GameVariant : unsigned {
    SelfOnly = 0,        // (a) eboot + sce_module/libc.prx as SELFs
    ElfOnly = 1 << 0,    // (b)
    Mixed = 1 << 1,      // (c) SELF eboot, ELF prx
    OtherFakelib = 1 << 2, // (d) another tool's fakelib + extra libSceAmpr.sprx
    EncryptedRight = 1 << 3, // (e) sce_sys/about/right.sprx encrypted
    OsMetadata = 1 << 4, // (f) ._eboot.bin, .DS_Store, __MACOSX/
    LeftoverBak = 1 << 5, // (g) eboot.bin.bak
    NestedEboot = 1 << 6, // (h) sub/eboot.bin
};

inline QByteArray paramJson(const QString& id = QStringLiteral("PPSA01234"), const QString& title = QStringLiteral("Game"))
{
    return QStringLiteral("{\n  \"titleId\": \"%1\",\n  \"contentVersion\": \"01.000.000\",\n"
                          "  \"sdkVersion\": \"0x0700000000000000\",\n"
                          "  \"requiredSystemSoftwareVersion\": \"0x0750000000000000\",\n"
                          "  \"localizedParameters\": {\"defaultLanguage\": \"en-US\", \"en-US\": {\"titleName\": \"%2\"}}\n}\n")
        .arg(id, title)
        .toUtf8();
}

inline void makeGame(const QString& root, unsigned v)
{
    QDir().mkpath(root);
    const bool elfOnly = v & ElfOnly;
    const bool mixed = v & Mixed;
    writeBytes(root + "/eboot.bin", elfOnly ? f1() : selfOf(f1()));
    writeBytes(root + "/sce_module/libc.prx", (elfOnly || mixed) ? f2() : selfOf(f2()));
    writeBytes(root + "/sce_sys/param.json", paramJson());
    writeBytes(root + "/sce_sys/icon0.png", QByteArray("\x89PNG not really", 15));
    writeBytes(root + "/data/level1.dat", QByteArray(5000, 'L'));
    if (v & OtherFakelib) {
        writeBytes(root + "/fakelib/libSceAgc.sprx", selfOf(fakeLib(77)));
        writeBytes(root + "/fakelib/libSceAmpr.sprx", selfOf(fakeLib(88)));
    }
    if (v & EncryptedRight)
        writeBytes(root + "/sce_sys/about/right.sprx", f7());
    if (v & OsMetadata) {
        writeBytes(root + "/._eboot.bin", QByteArray("\x00\x05\x16\x07", 4) + QByteArray(60, '\0'));
        writeBytes(root + "/.DS_Store", QByteArray("\0\0\0\1Bud1", 8));
        writeBytes(root + "/__MACOSX/sce_module/._libc.prx", QByteArray("\x00\x05\x16\x07", 4));
        writeBytes(root + "/sce_module/._libc.prx", QByteArray("\x00\x05\x16\x07", 4));
    }
    if (v & LeftoverBak)
        writeBytes(root + "/eboot.bin.bak", QByteArray("old backup"));
    if (v & NestedEboot)
        writeBytes(root + "/sub/eboot.bin", selfOf(f1()));
}

// rel → bytes of every file under `root`.
inline QMap<QString, QByteArray> snapshotTree(const QString& root)
{
    QMap<QString, QByteArray> out;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    const QDir base(root);
    while (it.hasNext()) {
        const QString p = it.next();
        out.insert(base.relativeFilePath(p), readBytes(p));
    }
    return out;
}

inline QStringList filesIn(const QString& dir)
{
    return QDir(dir).entryList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name);
}

struct Env {
    QTemporaryDir tmp;
    QString game, orig, bp, fakelib, temp, journals;
    fw::CancellationSource cancel;
    std::unique_ptr<fw::ProgressReporter> progress;
    fw::JobContext ctx;

    explicit Env(unsigned variant = SelfOnly, const QString& gameName = QStringLiteral("PPSA01234-app"))
    {
        game = tmp.filePath(QStringLiteral("games/") + gameName);
        orig = tmp.filePath(QStringLiteral("zips/orig"));
        bp = tmp.filePath(QStringLiteral("zips/bp"));
        fakelib = tmp.filePath(QStringLiteral("fakelib_set"));
        temp = tmp.filePath(QStringLiteral("temp/fw-job-1"));
        journals = tmp.filePath(QStringLiteral("journals"));
        for (const QString& d : {orig, bp, fakelib, temp, journals})
            QDir().mkpath(d);
        makeGame(game, variant);
        writeBytes(fakelib + "/libSceAgc.sprx", selfOf(fakeLib(11)));
        writeBytes(fakelib + "/libSceNpAuth.sprx", fakeLib(12));
        writeBytes(fakelib + "/readme.txt", QByteArray("notes"));
        progress = std::make_unique<fw::ProgressReporter>();
        progress->setPhases(fw::backport::runPhases());
        ctx.jobId = QStringLiteral("job-1");
        ctx.token = cancel.token();
        ctx.progress = progress.get();
        ctx.log = fw::Log(QStringLiteral("backport"), ctx.jobId);
        ctx.tempDir = temp;
    }

    fw::backport::BackportOptions options(bool withFakelib = true) const
    {
        fw::backport::BackportOptions o;
        o.game = game;
        o.targetSdk = 4;
        o.originalsDir = orig;
        o.backportDir = bp;
        o.fakelibDir = withFakelib ? fakelib : QString();
        o.applyToGame = true;
        o.journalDir = journals;
        o.freeSpaceMarginBytes = 0;
        return o;
    }
};

} // namespace fwtest::bp
