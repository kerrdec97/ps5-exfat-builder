// FPKG jobs end to end against the stand-in prospero-pub-cmd
// (tests/engines/fpkg/fake_pubcmd.cpp). Nothing here runs Sony's tools or a
// console: these tests prove what the engine does with a tool that behaves as
// described in native/docs/FPKG_PARITY.md section 4.

#include "FpkgTestUtil.h"

#include "core/Log.h"
#include "engines/fpkg/AsciiAliases.h"
#include "engines/fpkg/FpkgBuilder.h"
#include "engines/fpkg/Gp5.h"
#include "engines/fpkg/Jobs.h"
#include "engines/fpkg/PackageHeader.h"
#include "engines/fpkg/PubCmd.h"
#include "engines/fpkg/SourceStager.h"

#include "core/ProcessRunner.h"

#include <QDirIterator>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <mutex>
#include <thread>

using namespace fw;
using namespace fw::fpkg;
using namespace fpkgtest;

namespace {

const QString kAddonCid = QStringLiteral("UP0000-PPSA01234_00-DLCPACK000000001");
const QString kKey = QStringLiteral("0123456789ABCDEF0123456789abcdef");

QByteArray fullPackageBytes()
{
    QByteArray d(0x2000, '\0');
    d.replace(0, 4, QByteArray("\x7F" "FIH", 4));
    putLe64(d, 0x20, 0x1000);
    putLe16(d, 0x1000 + 0x1C, 13);
    d.replace(0x1000 + 0x370, 16, "PPRPLAIN-NOAUTH!");
    return d;
}

bool processAlive(qint64 pid)
{
#ifdef Q_OS_LINUX
    QFile f(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray stat = f.readAll();
    const qsizetype close = stat.lastIndexOf(')');
    return close > 0 && close + 2 < stat.size() && stat[close + 2] != 'Z';
#else
    return processIsRunning(pid);
#endif
}

// The request of a job, or an empty one (which the job refuses) with a
// warning: a parameter object that is not accepted must not end the program.
template <typename Request>
Request requestFrom(const QJsonObject& parameters)
{
    auto r = Request::fromJson(parameters);
    if (r.ok())
        return r.value();
    qWarning("The job parameters were not accepted: %s", qPrintable(r.error().toLogString()));
    return Request{};
}

QStringList argvOf(const QJsonObject& invocation)
{
    QStringList out;
    for (const auto& a : invocation.value(QStringLiteral("argv")).toArray())
        out << a.toString();
    return out;
}

// Copies a prepared tree into the destination (stands in for an image reader).
class FolderStager : public SourceStager {
public:
    explicit FolderStager(QString tree) : m_tree(std::move(tree)) {}
    QString displayName() const override { return QStringLiteral("test image"); }
    QStringList extensions() const override { return {QStringLiteral("exfat")}; }
    Status stage(const QString& image, const QString& destination, const CancellationToken& token,
                 ProgressReporter* progress) override
    {
        Q_UNUSED(image);
        ++calls;
        QDirIterator it(m_tree, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            if (!token.checkpoint())
                return Error::cancelled();
            const QString rel = QDir(m_tree).relativeFilePath(it.filePath());
            QDir().mkpath(QFileInfo(QDir(destination).filePath(rel)).absolutePath());
            if (!QFile::copy(it.filePath(), QDir(destination).filePath(rel)))
                return Error(ErrorCode::IoError, QStringLiteral("copy failed"));
            if (progress)
                progress->advance(it.fileInfo().size());
        }
        return success();
    }
    int calls = 0;

private:
    QString m_tree;
};

} // namespace

class TstFpkgBuild : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    QString m_tools;
    QString m_fakeLog;
    Env m_env;
    std::mutex m_logMutex;
    QStringList m_logLines;
    std::shared_ptr<CallbackSink> m_sink;
    std::atomic<int> m_counter{0};

    QString dir(const QString& name)
    {
        const QString p = m_root.filePath(name);
        QDir(p).removeRecursively();
        QDir().mkpath(p);
        return p;
    }

    QVariantMap baseSettings() const
    {
        return {{keys::kPubToolsPath, m_tools},
                {keys::kPlayGoChunkCount, 4},
                {general::keys::kFreeSpaceMarginMiB, 0}};
    }

    struct RunResult {
        Result<JobOutcome> result = Error(ErrorCode::Internal, QStringLiteral("not run"));
        std::vector<ProgressSnapshot> snapshots;
    };

    RunResult run(Job& job, const QVariantMap& extra = {}, const CancellationToken& token = {},
                  const QString& jobId = QString())
    {
        RunResult rr;
        QVariantMap settings = baseSettings();
        for (auto it = extra.begin(); it != extra.end(); ++it)
            settings.insert(it.key(), it.value());
        const SettingsValues values = m_env.values(settings);
        const Status valid = job.validate(values);
        if (!valid.ok()) {
            rr.result = valid.error();
            return rr;
        }
        std::mutex m;
        ProgressReporter reporter(
            [&](const ProgressSnapshot& s) {
                std::lock_guard lock(m);
                rr.snapshots.push_back(s);
            },
            0);
        reporter.setPhases(job.phases());
        const int n = ++m_counter;
        const QString temp = dir(QStringLiteral("jobtemp-%1").arg(n));
        JobContext ctx;
        ctx.jobId = jobId.isEmpty() ? QStringLiteral("J%1").arg(n) : jobId;
        ctx.token = token;
        ctx.progress = &reporter;
        ctx.log = Log(job.kind(), ctx.jobId);
        ctx.settings = values;
        ctx.tempDir = temp;
        rr.result = job.run(ctx);
        QDir(temp).removeRecursively(); // what the queue does
        return rr;
    }

    QList<QJsonObject> invocations()
    {
        QList<QJsonObject> out;
        for (const QByteArray& line : readAll(m_fakeLog).split('\n'))
            if (!line.trimmed().isEmpty())
                out << QJsonDocument::fromJson(line).object();
        return out;
    }

    QStringList filesIn(const QString& d)
    {
        QStringList out;
        QDirIterator it(d, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext())
            out << QDir(d).relativeFilePath(it.next());
        out.sort();
        return out;
    }

    bool logged(const QString& needle)
    {
        std::lock_guard lock(m_logMutex);
        return std::any_of(m_logLines.begin(), m_logLines.end(), [&](const QString& l) { return l.contains(needle); });
    }

    FpkgBuildJob buildJob(const QString& source, const QString& output, const QJsonObject& extra = {})
    {
        QJsonObject p = extra;
        p.insert(QStringLiteral("source"), source);
        if (!output.isEmpty())
            p.insert(QStringLiteral("output"), output);
        return FpkgBuildJob(requestFrom<BuildRequest>(p));
    }

    // The ASCII links of this process that still exist, in every folder the
    // engine may put them (the redirected temporary folder and %ProgramData%,
    // and the drive's root as the last resort).
    QStringList aliasLinksLeft() const
    {
        const QString mine = QStringLiteral("a%1-").arg(QCoreApplication::applicationPid(), 0, 16);
        QStringList left;
        for (const QString& base : {QDir::tempPath(),
                                    QDir(qEnvironmentVariable("ProgramData")).filePath(QStringLiteral("fw-fpkg")),
                                    QDir(m_root.path()).rootPath()}) {
            const QDir aliases(QDir(base).filePath(AsciiAliases::aliasFolderName()));
            for (const QString& n : aliases.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System | QDir::Hidden))
                if (n.startsWith(mine))
                    left << aliases.filePath(n);
        }
        return left;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        QVERIFY(confineEngineTo(m_root.path()));
        m_tools = dir(QStringLiteral("tools"));
        const QString exe = QDir(m_tools).filePath(QStringLiteral("toolchain/prospero-pub-cmd.exe"));
        QDir().mkpath(QDir(m_tools).filePath(QStringLiteral("toolchain/ext")));
        QVERIFY(QFile::copy(QStringLiteral(FW_FAKE_PUBCMD), exe));
        QVERIFY(QFile::copy(QStringLiteral(FW_FAKE_PUBCMD), QDir(m_tools).filePath(QStringLiteral("prospero-dds2png.exe"))));
        QVERIFY(put(m_tools, QStringLiteral("toolchain/libScePubTools.dll"), ""));
        m_fakeLog = m_root.filePath(QStringLiteral("fake.log"));
        setEnv("FAKE_PUBCMD_LOG", m_fakeLog);
        m_sink = std::make_shared<CallbackSink>([this](const LogRecord& r) {
            std::lock_guard lock(m_logMutex);
            m_logLines << r.message;
        });
        Logger::instance().addSink(m_sink);
    }

    void init()
    {
        QFile::remove(m_fakeLog);
        std::lock_guard lock(m_logMutex);
        m_logLines.clear();
    }

    void cleanupTestCase() { Logger::instance().removeSink(m_sink); }

    void fullBuild()
    {
        const QString game = dir(QStringLiteral("full/PPSA01234-app"));
        const QString out = m_root.filePath(QStringLiteral("full/out"));
        QVERIFY(makeSimpleDump(game));
        put(game, QStringLiteral("Thumbs.db"), "junk");
        auto job = buildJob(game, out);
        auto rr = run(job);
        QVERIFY2(rr.result.ok(), qPrintable(rr.result.ok() ? QString() : rr.result.error().toLogString()));
        const JobOutcome& o = rr.result.value();
        const QString pkg = QDir(out).filePath(kStem + QStringLiteral(".pkg"));
        QCOMPARE(o.outputs, QStringList{pkg});
        QVERIFY(checkPackageHeader(pkg).ok);
        QCOMPARE(o.verification, Verification::Passed);
        QVERIFY(o.verificationDetail.contains(QStringLiteral("img_verify passed")));
        // Only the package and the logs are left; partials, the project and the metric are gone.
        QCOMPARE(filesIn(out), (QStringList{kStem + QStringLiteral("-build-logs/01-create-gp5.log"),
                                            kStem + QStringLiteral("-build-logs/02-img-create.log"),
                                            kStem + QStringLiteral(".pkg")}));
        const QByteArray log02 = readAll(QDir(out).filePath(kStem + QStringLiteral("-build-logs/02-img-create.log")));
        QVERIFY(log02.startsWith("command="));
        QVERIFY(log02.contains("\nexit_code=0\n"));
        QVERIFY(log02.contains("\nelapsed=00:00:"));
        const QByteArray log01 = readAll(QDir(out).filePath(kStem + QStringLiteral("-build-logs/01-create-gp5.log")));
        QVERIFY(log01.contains("excluded: sce_sys/param.sfo (generated by the SDK)"));
        QVERIFY(log01.contains("excluded: Thumbs.db (OS metadata file)"));
        QVERIFY(log01.contains("empty_dir: addcont0"));
        QVERIFY(log01.contains("size: estimated"));

        const auto inv = invocations();
        QCOMPARE(inv.size(), 2); // img_create, img_verify
        const QJsonArray argv = inv[0].value(QStringLiteral("argv")).toArray();
        QCOMPARE(argv.at(0).toString(), QStringLiteral("img_create"));
        QCOMPARE(argv.at(1).toString(), QStringLiteral("--oformat"));
        QCOMPARE(argv.at(2).toString(), QStringLiteral("nwonly"));
        QCOMPARE(argv.size(), 5); // tool default: no --compression_level
        QVERIFY(argv.at(4).toString().endsWith(kStem + QStringLiteral(".partial.pkg")));
        const QString temp = inv[0].value(QStringLiteral("TEMP")).toString();
        QCOMPARE(inv[0].value(QStringLiteral("TMP")).toString(), temp);
        // (A long temporary path is given to the tool in its 8.3 spelling.)
        const QString tempName = QFileInfo(QDir::fromNativeSeparators(temp)).fileName();
        QVERIFY2(tempName.startsWith(QStringLiteral("sdk-tmp-")) || tempName.startsWith(QStringLiteral("SDK-TM~"), Qt::CaseInsensitive),
                 qPrintable(temp));
        QVERIFY(!QFileInfo::exists(temp)); // removed after the run
        QCOMPARE(QDir::cleanPath(inv[0].value(QStringLiteral("cwd")).toString()),
                 QDir::cleanPath(QFileInfo(QDir(m_tools).filePath(QStringLiteral("toolchain"))).canonicalFilePath()));
        QCOMPARE(inv[1].value(QStringLiteral("argv")).toArray().at(0).toString(), QStringLiteral("img_verify"));
        QVERIFY(inv[1].value(QStringLiteral("argv")).toArray().contains(QStringLiteral("on")));

        // Progress: the pack phase reached the end of the bar; the pre-scan saw every game file.
        bool packed = false, prescanned = false;
        for (const auto& s : rr.snapshots) {
            if (s.phaseIndex == 4 && s.done == 1000)
                packed = true;
            if (s.phaseIndex == 3 && s.filesTotal == 5)
                prescanned = true;
        }
        QVERIFY(packed);
        QVERIFY(prescanned);
    }

    void projectContentsAndCompression()
    {
        const QString game = dir(QStringLiteral("proj/game"));
        const QString out = m_root.filePath(QStringLiteral("proj/out"));
        QVERIFY(makeSimpleDump(game));
        auto job = buildJob(game, out);
        auto rr = run(job, {{keys::kKeepIntermediate, true}, {keys::kCompressionLevel, QStringLiteral("9")},
                            {keys::kPostVerify, QStringLiteral("toolFormat")}});
        QVERIFY2(rr.result.ok(), qPrintable(rr.result.ok() ? QString() : rr.result.error().toLogString()));
        const QString gp5 = QDir(out).filePath(kStem + QStringLiteral(".gp5"));
        auto project = readGp5(gp5);
        QVERIFY(project.ok());
        QCOMPARE(project->files.front().dst, QStringLiteral("sce_sys/playgo-scenario.json"));
        QCOMPARE(QDir::cleanPath(project->files.front().src), QDir::cleanPath(QDir(out).filePath(kStem + QStringLiteral(".playgo-scenario.json"))));
        QString paramSrc;
        int payloads = 0;
        for (const auto& f : project->files) {
            if (f.dst == u"sce_sys/param.json")
                paramSrc = f.src;
            if (f.noCompression) {
                ++payloads;
                QCOMPARE(QFileInfo(f.src).size(), kPayloadBytes);
            }
            QVERIFY(f.dst != u"sce_sys/param.sfo");
        }
        QCOMPARE(payloads, 3); // the game data in chunk 0, the en-US payload in chunk 1, fillers for chunks 2-3
        QVERIFY(paramSrc.contains(QStringLiteral(".gp5-assets")));
        QVERIFY(readAll(paramSrc).contains("\"applicationDrmType\": \"standard\""));
        QCOMPARE(project->emptyDirs, QStringList{QStringLiteral("addcont0")});
        QVERIFY(QFileInfo::exists(QDir(out).filePath(kStem + QStringLiteral(".playgo-scenario.json"))));
        const auto inv = invocations();
        QCOMPARE(inv.size(), 2);
        const QJsonArray argv = inv[0].value(QStringLiteral("argv")).toArray();
        QCOMPARE(argv.at(3).toString(), QStringLiteral("--compression_level"));
        QCOMPARE(argv.at(4).toString(), QStringLiteral("9"));
        QVERIFY(inv[1].value(QStringLiteral("argv")).toArray().contains(QStringLiteral("off"))); // format only
        // Header only: no img_verify at all.
        QFile::remove(m_fakeLog);
        auto again = run(job, {{keys::kPostVerify, QStringLiteral("headerOnly")}, {keys::kOutputConflict, QStringLiteral("overwrite")}});
        QVERIFY(again.result.ok());
        QCOMPARE(invocations().size(), 1);
        QVERIFY(again.result->verificationDetail.contains(QStringLiteral("Header")));
    }

    void toolFailuresAreFailures_data()
    {
        QTest::addColumn<QByteArray>("mode");
        QTest::addColumn<int>("code");
        QTest::addColumn<QString>("needle");
        QTest::newRow("stock") << QByteArray("stock") << int(ErrorCode::VerificationFailed) << QStringLiteral("marker");
        QTest::newRow("error-exit-0") << QByteArray("error") << int(ErrorCode::ExternalToolFailed) << QStringLiteral("8-bit RGBA");
        QTest::newRow("crash") << QByteArray("crash") << int(ErrorCode::ExternalToolFailed) << QStringLiteral("exit code 3");
        QTest::newRow("nooutput") << QByteArray("nooutput") << int(ErrorCode::ExternalToolFailed) << QStringLiteral("wrote no package");
#ifdef Q_OS_WIN
        QTest::newRow("ntstatus") << QByteArray("ntstatus") << int(ErrorCode::DependencyMissing) << QStringLiteral("Visual C++");
        QTest::newRow("badimage") << QByteArray("badimage") << int(ErrorCode::DependencyMissing) << QStringLiteral("architecture");
#endif
    }

    void toolFailuresAreFailures()
    {
        QFETCH(QByteArray, mode);
        QFETCH(int, code);
        QFETCH(QString, needle);
        EnvVar env("FAKE_PUBCMD_MODE", mode);
        const QString game = dir(QStringLiteral("fail/game"));
        const QString out = m_root.filePath(QStringLiteral("fail/out-") + QString::fromLatin1(mode));
        QVERIFY(makeSimpleDump(game));
        auto job = buildJob(game, out);
        auto rr = run(job);
        QVERIFY(!rr.result.ok());
        QCOMPARE(int(rr.result.error().code), code);
        QVERIFY2(errorOf(rr.result).message.contains(needle), qPrintable(errorOf(rr.result).message));
        // No package, no partials; the project and the logs are kept for diagnosis.
        for (const QString& f : filesIn(out)) {
            QVERIFY2(!f.endsWith(QStringLiteral(".pkg")) && !f.contains(QStringLiteral(".partial")), qPrintable(f));
        }
        QVERIFY(QFileInfo::exists(QDir(out).filePath(kStem + QStringLiteral(".gp5"))));
        QVERIFY(QFileInfo::exists(QDir(out).filePath(kStem + QStringLiteral("-build-logs/02-img-create.log"))));
    }

    void barVariantsAndMissingKeystoneMessage()
    {
        const QString game = dir(QStringLiteral("bars/game"));
        QVERIFY(makeSimpleDump(game));
        for (const QByteArray& mode : {QByteArray("split"), QByteArray("cr"), QByteArray("nobar")}) {
            EnvVar env("FAKE_PUBCMD_MODE", mode);
            const QString out = m_root.filePath(QStringLiteral("bars/out-") + QString::fromLatin1(mode));
            auto job = buildJob(game, out);
            auto rr = run(job);
            QVERIFY2(rr.result.ok(), mode.constData());
            bool full = false, partWay = false;
            for (const auto& s : rr.snapshots) {
                full = full || (s.phaseIndex == 4 && s.done == 1000);
                partWay = partWay || (s.phaseIndex == 4 && s.done > 0 && s.done < 1000);
            }
            // Without a bar there is nothing to count: no steps are invented.
            if (mode == "nobar")
                QVERIFY2(!partWay, "bar steps were reported for a tool that printed no bar");
            else
                QVERIFY2(full, mode.constData());
        }
        // The tool's own keystone error is explained (a .gp5 project without a keystone entry
        // never reaches it, so drive the parser directly).
        QVERIFY(explainToolErrors({QStringLiteral("The keystone file is missing (sce_sys/keystone)")}).join(u' ').contains(QStringLiteral("96-byte")));
    }

    void verifyFailureKeepsPackage()
    {
        EnvVar env("FAKE_PUBCMD_VERIFY", "fail");
        const QString game = dir(QStringLiteral("vfail/game"));
        const QString out = m_root.filePath(QStringLiteral("vfail/out"));
        QVERIFY(makeSimpleDump(game));
        auto job = buildJob(game, out);
        auto rr = run(job);
        QVERIFY(rr.result.ok());
        QCOMPARE(rr.result->verification, Verification::Inconclusive);
        QVERIFY(QFileInfo::exists(rr.result->outputs.first()));
        QVERIFY(std::any_of(rr.result->warnings.begin(), rr.result->warnings.end(),
                            [](const QString& w) { return w.contains(QStringLiteral("img_verify")); }));
    }

    // "Overwrite" deletes the old package only after img_verify passed; when it
    // does not, the old one is back under its name and the new one is kept.
    void overwriteKeepsTheOldPackageUntilVerified()
    {
        const QString game = dir(QStringLiteral("overwrite/game"));
        const QString out = m_root.filePath(QStringLiteral("overwrite/out"));
        QVERIFY(makeSimpleDump(game));
        auto job = buildJob(game, out);
        const QVariantMap overwrite{{keys::kOutputConflict, QStringLiteral("overwrite")}};
        const QString pkg = QDir(out).filePath(kStem + QStringLiteral(".pkg"));
        const QString kept = QDir(out).filePath(kStem + QStringLiteral(" (1).pkg"));
        const auto asideLeft = [&] {
            const QStringList files = filesIn(out);
            return std::any_of(files.begin(), files.end(), [](const QString& f) { return f.contains(QStringLiteral(".fw-replaced")); });
        };
        put(out, kStem + QStringLiteral(".pkg"), "previous good");
        {
            EnvVar env("FAKE_PUBCMD_VERIFY", "fail");
            auto rr = run(job, overwrite);
            QVERIFY2(rr.result.ok(), qPrintable(rr.result.ok() ? QString() : rr.result.error().toLogString()));
            QCOMPARE(rr.result->verification, Verification::Inconclusive);
            QCOMPARE(readAll(pkg), QByteArray("previous good"));
            QCOMPARE(rr.result->outputs, QStringList{kept});
            QVERIFY(checkPackageHeader(kept).ok);
            QVERIFY(!asideLeft());
            QVERIFY(std::any_of(rr.result->warnings.begin(), rr.result->warnings.end(),
                                [](const QString& w) { return w.contains(QStringLiteral("was put back")); }));
        }
        auto rr = run(job, overwrite);
        QVERIFY(rr.result.ok());
        QCOMPARE(rr.result->verification, Verification::Passed);
        QCOMPARE(rr.result->outputs, QStringList{pkg});
        QVERIFY(checkPackageHeader(pkg).ok);
        QVERIFY(!asideLeft());
        QVERIFY(std::any_of(rr.result->warnings.begin(), rr.result->warnings.end(),
                            [](const QString& w) { return w.contains(QStringLiteral("was replaced")); }));
    }

    // The required integrity pass of the backport flow fails on an [Error]
    // line even when the tool exits with 0.
    void publisherVerifyFailsOnErrorLines()
    {
        const QString d = dir(QStringLiteral("pubverify"));
        put(d, QStringLiteral("base.pkg"), fullPackageBytes());
        const SettingsValues values = m_env.values(baseSettings());
        const FpkgSettings s = FpkgSettings::from(values);
        const PubToolchain tc = locatePubTools(s.pubToolsPath);
        QVERIFY(tc.ok());
        ProgressReporter reporter([](const ProgressSnapshot&) {}, 0);
        JobContext ctx;
        ctx.jobId = QStringLiteral("Jverify");
        ctx.progress = &reporter;
        ctx.log = Log(QStringLiteral("fpkg.backport"), ctx.jobId);
        ctx.settings = values;
        ctx.tempDir = dir(QStringLiteral("pubverify/temp"));
        const QString pkg = QDir(d).filePath(QStringLiteral("base.pkg"));
        auto good = verifyPublisherPackage(pkg, tc, s, ctx);
        QVERIFY2(good.ok(), qPrintable(good.ok() ? QString() : good.error().toLogString()));
        QVERIFY(good->integrityPassed && good->formatPassed);
        EnvVar soft("FAKE_PUBCMD_VERIFY", "softfail");
        auto bad = verifyPublisherPackage(pkg, tc, s, ctx);
        QVERIFY(!bad.ok());
        QCOMPARE(errorOf(bad).code, ErrorCode::VerificationFailed);
    }

    // A build that fails before it wrote anything does not leave its new,
    // empty output folder next to the game.
    void failedBuildLeavesNoEmptyOutputFolder()
    {
        const QString game = dir(QStringLiteral("emptyout/game"));
        const QString out = m_root.filePath(QStringLiteral("emptyout/out"));
        QVERIFY(makeSimpleDump(game));
        QFile::remove(QDir(game).filePath(QStringLiteral("sce_sys/keystone")));
        auto job = buildJob(game, out);
        auto rr = run(job);
        QVERIFY(!rr.result.ok());
        QCOMPARE(errorOf(rr.result).code, ErrorCode::CorruptData);
        QVERIFY(!QFileInfo::exists(out));
        // A folder that was there before stays, empty or not.
        QDir().mkpath(out);
        QVERIFY(!run(job).result.ok());
        QVERIFY(QFileInfo(out).isDir());
    }

    // The queue must not run another job on the files these jobs read or write.
    void busyPathsCoverBaseAndOutput()
    {
        const QString d = dir(QStringLiteral("busy"));
        const auto abs = [&](const char* rel) { return QDir::cleanPath(QDir(d).filePath(QString::fromLatin1(rel))); };
        QDir().mkpath(abs("patch"));
        auto update = buildJob(abs("patch"), QString(),
                               {{QStringLiteral("basePackage"), abs("base.pkg")}, {QStringLiteral("baseFolder"), abs("base-game")}});
        const QStringList busy = update.busyPaths();
        for (const char* rel : {"patch", "patch-pkg", "base.pkg", "base-game"})
            QVERIFY2(busy.contains(abs(rel)), rel);
        AddonRequest ar;
        ar.payloadFolder = abs("addon");
        ar.output = abs("addon-out");
        ar.entitlementKey = kKey;
        const QStringList addonBusy = FpkgAddonJob(ar).busyPaths();
        QVERIFY(addonBusy.contains(abs("addon")));
        QVERIFY(addonBusy.contains(abs("addon-out")));
    }

    void outputConflictPolicies()
    {
        const QString game = dir(QStringLiteral("conflict/game"));
        const QString out = m_root.filePath(QStringLiteral("conflict/out"));
        QVERIFY(makeSimpleDump(game));
        auto job = buildJob(game, out);
        QVERIFY(run(job).result.ok());
        const QString pkg = QDir(out).filePath(kStem + QStringLiteral(".pkg"));
        put(out, kStem + QStringLiteral(".pkg"), "previous");
        auto keep = run(job);
        QVERIFY(keep.result.ok());
        QCOMPARE(readAll(QDir(out).filePath(kStem + QStringLiteral(" (1).pkg"))), QByteArray("previous"));
        QVERIFY(checkPackageHeader(pkg).ok);
        QVERIFY(std::any_of(keep.result->warnings.begin(), keep.result->warnings.end(),
                            [](const QString& w) { return w.contains(QStringLiteral("(1).pkg")); }));

        put(out, kStem + QStringLiteral(".pkg"), "replace me");
        auto over = run(job, {{keys::kOutputConflict, QStringLiteral("overwrite")}});
        QVERIFY(over.result.ok());
        QVERIFY(checkPackageHeader(pkg).ok);
        QVERIFY(!QFileInfo::exists(QDir(out).filePath(kStem + QStringLiteral(" (2).pkg"))));

        put(out, kStem + QStringLiteral(".pkg"), "keep me");
        QFile::remove(m_fakeLog);
        auto stop = run(job, {{keys::kOutputConflict, QStringLiteral("fail")}});
        QCOMPARE(errorOf(stop.result).code, ErrorCode::AlreadyExists);
        QCOMPARE(readAll(pkg), QByteArray("keep me"));
        QVERIFY(invocations().isEmpty()); // refused before packing
    }

    // The update is built against the package given as its base: the stand-in
    // writes the base's content digest into the update, as the real tool does.
    void updateCarriesTheDigestOfItsBase()
    {
        const QString game = dir(QStringLiteral("digest/PPSA01234-app"));
        const QString baseOut = m_root.filePath(QStringLiteral("digest/base-out"));
        QVERIFY(makeSimpleDump(game));
        auto baseJob = buildJob(game, baseOut);
        auto built = run(baseJob);
        QVERIFY2(built.result.ok(), qPrintable(built.result.ok() ? QString() : built.result.error().toLogString()));
        const QString basePkg = built.result->outputs.value(0);
        const QByteArray digest = readAll(basePkg).mid(0x30, 32);
        QVERIFY(digest.size() == 32 && digest != QByteArray(32, '\0'));

        const QString update = dir(QStringLiteral("digest/patch"));
        QVERIFY(put(update, QStringLiteral("sce_sys/param.json"),
                    QStringLiteral("{\"contentId\":\"%1\",\"contentVersion\":\"01.001.000\",\"applicationCategoryType\":0}").arg(kCid).toUtf8()));
        QVERIFY(put(update, QStringLiteral("Data/A.pak"), QByteArray(10, 'u')));
        QFile::remove(m_fakeLog);
        auto job = buildJob(update, m_root.filePath(QStringLiteral("digest/out")),
                            QJsonObject{{QStringLiteral("basePackage"), basePkg}, {QStringLiteral("baseFolder"), game}});
        auto rr = run(job);
        QVERIFY2(rr.result.ok(), qPrintable(rr.result.ok() ? QString() : rr.result.error().toLogString()));
        QCOMPARE(rr.result->outputs.size(), 2);
        const QByteArray delta = readAll(rr.result->outputs[0]);
        QVERIFY(delta.startsWith(QByteArray("\x7F" "LIH", 4)));
        QVERIFY(delta.contains(digest));
        const auto inv = invocations();
        QVERIFY(!inv.isEmpty());
        const QStringList argv = argvOf(inv.first());
        const qsizetype at = argv.indexOf(QStringLiteral("--ref_pkg_path"));
        QVERIFY(at > 0 && at + 1 < argv.size());
        QCOMPARE(QDir::cleanPath(QDir::fromNativeSeparators(argv.at(at + 1))), QDir::cleanPath(basePkg));

        // A base the tool did not build: its own message is explained to the user.
        EnvVar mode("FAKE_PUBCMD_MODE", "nonaps");
        auto refused = run(job, {{keys::kOutputConflict, QStringLiteral("overwrite")}});
        QCOMPARE(errorOf(refused.result).code, ErrorCode::ExternalToolFailed);
        QVERIFY2(errorOf(refused.result).message.contains(QStringLiteral("not built by the Publishing Tools")),
                 qPrintable(errorOf(refused.result).message));
    }

    // The finished package cannot always take its name (on Windows the old one
    // may be open in another program). It is then kept under a free numbered
    // name, or as its .partial.pkg, and the error says where: it is never
    // deleted. An update and its .remastered.pkg move together, so the old
    // pair is never left half replaced.
    void placementFailureKeepsNewPackages()
    {
        // With every "name (n).pkg" taken, "Keep both" cannot move the existing
        // file aside: a failure every platform can reproduce.
        const auto takeNumberedNames = [](const QString& finalPath) {
            const QFileInfo fi(finalPath);
            for (int n = 1; n < 10000; ++n) {
                QFile f(fi.dir().filePath(numberedName(fi.fileName(), n)));
                if (!f.open(QIODevice::WriteOnly))
                    return false;
            }
            return true;
        };

        // A full package: the existing one stays, the new one is kept as its partial file.
        const QString game = dir(QStringLiteral("place/game"));
        const QString out = dir(QStringLiteral("place/out"));
        QVERIFY(makeSimpleDump(game));
        const QString pkg = QDir(out).filePath(kStem + QStringLiteral(".pkg"));
        QVERIFY(put(out, QFileInfo(pkg).fileName(), "previous"));
        QVERIFY(takeNumberedNames(pkg));
        auto job = buildJob(game, out);
        auto rr = run(job);
        QVERIFY(!rr.result.ok());
        QCOMPARE(errorOf(rr.result).code, ErrorCode::AlreadyExists);
        const QString keptPkg = QDir(out).filePath(kStem + QStringLiteral(".partial.pkg"));
        QVERIFY2(errorOf(rr.result).message.contains(QDir::toNativeSeparators(keptPkg)),
                 qPrintable(errorOf(rr.result).toLogString()));
        QVERIFY(checkPackageHeader(keptPkg).ok);
        QCOMPARE(readAll(pkg), QByteArray("previous"));
        QVERIFY(logged(QStringLiteral("The finished package was kept as")));

        // An update: the old UPDATE_ package could be moved aside, the old
        // .remastered.pkg could not. Neither is replaced; the new update is
        // kept under the first free number, its .remastered.pkg as a partial.
        const QString base = dir(QStringLiteral("place/base-game"));
        QVERIFY(makeSimpleDump(base));
        QVERIFY(put(m_root.filePath(QStringLiteral("place/base")), QStringLiteral("base.pkg"), fullPackageBytes()));
        const QString update = dir(QStringLiteral("place/patch"));
        put(update, QStringLiteral("sce_sys/param.json"),
            QStringLiteral("{\"contentId\":\"%1\",\"contentVersion\":\"01.001.000\",\"applicationCategoryType\":0}").arg(kCid).toUtf8());
        put(update, QStringLiteral("Data/A.pak"), QByteArray(10, 'u'));
        const QString out2 = dir(QStringLiteral("place/out-update"));
        const QString stem = kCid + QStringLiteral("-A0101-V0101");
        const QString updPkg = QDir(out2).filePath(QStringLiteral("UPDATE_") + stem + QStringLiteral(".pkg"));
        const QString fullPkg = QDir(out2).filePath(stem + QStringLiteral(".remastered.pkg"));
        QVERIFY(put(out2, QFileInfo(updPkg).fileName(), "old update"));
        QVERIFY(put(out2, QFileInfo(fullPkg).fileName(), "old full"));
        QVERIFY(takeNumberedNames(fullPkg));
        auto ujob = buildJob(update, out2,
                             QJsonObject{{QStringLiteral("basePackage"), m_root.filePath(QStringLiteral("place/base/base.pkg"))},
                                         {QStringLiteral("baseFolder"), base}});
        auto ur = run(ujob);
        QVERIFY(!ur.result.ok());
        const QString msg = ur.result.error().message;
        QCOMPARE(readAll(updPkg), QByteArray("old update"));
        QCOMPARE(readAll(fullPkg), QByteArray("old full"));
        const QString keptUpd = QDir(out2).filePath(QStringLiteral("UPDATE_") + stem + QStringLiteral(" (1).pkg"));
        const QString keptFull = QDir(out2).filePath(stem + QStringLiteral(".partial.pkg.remastered.pkg"));
        QVERIFY2(msg.contains(QDir::toNativeSeparators(keptUpd)), qPrintable(msg));
        QVERIFY2(msg.contains(QDir::toNativeSeparators(keptFull)), qPrintable(msg));
        QVERIFY(QFileInfo(keptUpd).size() > 0);
        QVERIFY(!checkPackageHeader(keptUpd).hasFih); // the delta
        QVERIFY(checkPackageHeader(keptFull).ok);
        QVERIFY(!QFileInfo::exists(QDir(out2).filePath(stem + QStringLiteral(".partial.pkg"))));
    }

    void cancelKillsTreeAndCleansUp()
    {
        EnvVar mode("FAKE_PUBCMD_MODE", "slow");
        const QString pidFile = m_root.filePath(QStringLiteral("child.pid"));
        const QString ticks = m_root.filePath(QStringLiteral("cancel.ticks"));
        QFile::remove(pidFile);
        QFile::remove(ticks);
        EnvVar child("FAKE_PUBCMD_CHILD_PIDFILE", pidFile.toUtf8());
        EnvVar tick("FAKE_PUBCMD_TICKFILE", ticks.toUtf8());
        const QString game = dir(QStringLiteral("cancel/game"));
        const QString out = m_root.filePath(QStringLiteral("cancel/out"));
        QVERIFY(makeSimpleDump(game));
        auto job = buildJob(game, out);
        CancellationSource cancel;
        std::thread canceller([&] {
            for (int i = 0; i < 200 && !QFileInfo::exists(QDir(out).filePath(kStem + QStringLiteral(".partial.pkg"))); ++i)
                QThread::msleep(50);
            QThread::msleep(300);
            cancel.cancel(QStringLiteral("test"));
        });
        auto rr = run(job, {}, cancel.token());
        canceller.join();
        QVERIFY(!rr.result.ok());
        QVERIFY(errorOf(rr.result).isCancelled());
        // The tool counts its bar steps into the tick file: it was stopped before the last one.
        const qint64 steps = QFileInfo(ticks).size();
        QVERIFY2(steps > 10 && steps < kBarSteps, qPrintable(QStringLiteral("img_create was not stopped (%1 steps)").arg(steps)));
        for (const QString& f : filesIn(out))
            QVERIFY2(!f.contains(QStringLiteral(".partial")) && !f.endsWith(QStringLiteral(".part")) && !f.endsWith(QStringLiteral(".pkg")),
                     qPrintable(f));
        QVERIFY(readAll(QDir(out).filePath(kStem + QStringLiteral("-build-logs/02-img-create.log"))).contains("exit_code=cancelled"));
        QVERIFY(QFileInfo::exists(QDir(out).filePath(kStem + QStringLiteral(".gp5"))));
        bool ok = false;
        const qint64 pid = readAll(pidFile).trimmed().toLongLong(&ok);
        QVERIFY2(ok, "the stand-in did not start its child");
        bool gone = false;
        for (int i = 0; i < 100 && !gone; ++i) {
            gone = !processAlive(pid);
            if (!gone)
                QThread::msleep(50);
        }
        QVERIFY2(gone, "the child of the tool survived the cancel");
    }

    void cpuLimitAppliedAndLive()
    {
        if (QThread::idealThreadCount() < 3)
            QSKIP("needs at least 3 logical processors");
        std::mutex m;
        QList<int> calls;
        setCpuLimitApplier([&](qint64 pid, int cores) {
            {
                std::lock_guard lock(m);
                calls << cores;
            }
            return setProcessCpuLimit(pid, cores);
        });
        EnvVar delay("FAKE_PUBCMD_DELAY_MS", "60");
        const QString game = dir(QStringLiteral("cpu/game"));
        const QString out = m_root.filePath(QStringLiteral("cpu/out"));
        QVERIFY(makeSimpleDump(game));
        auto job = buildJob(game, out);
        const QString id = QStringLiteral("Jcpu");
        std::thread changer([&] {
            for (int i = 0; i < 200 && !QFileInfo::exists(QDir(out).filePath(kStem + QStringLiteral(".partial.pkg"))); ++i)
                QThread::msleep(25);
            setLiveCpuLimit(id, 2);
        });
        auto rr = run(job, {{general::keys::kWorkerThreads, 1}}, {}, id);
        changer.join();
        setCpuLimitApplier({});
        QVERIFY(rr.result.ok());
        {
            std::lock_guard lock(m);
            QVERIFY2(calls.size() >= 2, qPrintable(QStringLiteral("%1 calls").arg(calls.size())));
            QCOMPARE(calls.first(), 1);
            QCOMPARE(calls.last(), 2);
        }
        QVERIFY(!liveCpuLimit(id)); // cleared when the job ends
        QVERIFY(!invocations().isEmpty());
        const QJsonArray samples = invocations().first().value(QStringLiteral("cpuSamples")).toArray();
        QVERIFY(samples.size() >= 3);
        QVERIFY(std::any_of(samples.begin(), samples.end(), [](const QJsonValue& v) { return v.toInt() == 1; }));
        QCOMPARE(samples.last().toInt(), 2);
    }

    // Performance mode, the core limit, a live limit and the private TEMP
    // apply to every publisher run of a job, not only to img_create.
    void limitsApplyToEveryPublisherRun()
    {
        if (QThread::idealThreadCount() < 3)
            QSKIP("needs at least 3 logical processors");
        const QString game = dir(QStringLiteral("limits/game"));
        const QString out = m_root.filePath(QStringLiteral("limits/out"));
        QVERIFY(makeSimpleDump(game));
        auto job = buildJob(game, out);
        const QVariantMap low{{general::keys::kWorkerThreads, 1},
                              {general::keys::kPerformanceMode, QStringLiteral("lowImpact")}};
        const QString ownTemp = QDir::toNativeSeparators(qEnvironmentVariable("TEMP"));
        const auto checkRun = [&](const QJsonObject& r, const QString& command, int cpus) {
            QCOMPARE(r.value(QStringLiteral("argv")).toArray().first().toString(), command);
            QCOMPARE(r.value(QStringLiteral("cpus")).toInt(), cpus);
#ifdef Q_OS_WIN
            QCOMPARE(r.value(QStringLiteral("priority")).toInt(), 0x40); // idle
            const QString temp = r.value(QStringLiteral("TEMP")).toString();
            QVERIFY(!temp.isEmpty());
            QCOMPARE(r.value(QStringLiteral("TMP")).toString(), temp);
            QVERIFY2(temp.compare(ownTemp, Qt::CaseInsensitive) != 0, qPrintable(temp));
#endif
        };

        QVERIFY(run(job, low).result.ok());
        QList<QJsonObject> runs = invocations();
        QCOMPARE(runs.size(), 2);
        checkRun(runs[0], QStringLiteral("img_create"), 1);
        checkRun(runs[1], QStringLiteral("img_verify"), 1);
        QCOMPARE(runs[1].value(QStringLiteral("TEMP")), runs[0].value(QStringLiteral("TEMP")));

        // A live limit set for the job reaches its later runs too, and is forgotten when the job ends.
        QFile::remove(m_fakeLog);
        const QString id = QStringLiteral("Jlive");
        setLiveCpuLimit(id, 2);
        QVERIFY(run(job, low, {}, id).result.ok());
        runs = invocations();
        QCOMPARE(runs.size(), 2);
        checkRun(runs[0], QStringLiteral("img_create"), 2);
        checkRun(runs[1], QStringLiteral("img_verify"), 2);
        QVERIFY(!liveCpuLimit(id));

        // The verify and extract jobs.
        QFile::remove(m_fakeLog);
        VerifyRequest vr;
        vr.package = QDir(out).filePath(kStem + QStringLiteral(".pkg"));
        FpkgVerifyJob verify(vr);
        QVERIFY(run(verify, low).result.ok());
        ExtractRequest er;
        er.package = vr.package;
        er.destination = m_root.filePath(QStringLiteral("limits/extracted"));
        FpkgExtractJob extract(er);
        QVERIFY(run(extract, low).result.ok());
        runs = invocations();
        QCOMPARE(runs.size(), 2);
        checkRun(runs[0], QStringLiteral("img_verify"), 1);
        checkRun(runs[1], QStringLiteral("img_extract"), 1);
    }

    // Pause suspends the tool and the processes it started: nothing moves until Resume.
    void pauseSuspendsTheToolTree()
    {
        EnvVar delay("FAKE_PUBCMD_DELAY_MS", "60");
        const QString ticks = m_root.filePath(QStringLiteral("pause.ticks"));
        const QString pidFile = m_root.filePath(QStringLiteral("pause-child.pid"));
        const QString childTicks = pidFile + QStringLiteral(".ticks");
        for (const QString& f : {ticks, pidFile, childTicks})
            QFile::remove(f);
        EnvVar tick("FAKE_PUBCMD_TICKFILE", ticks.toUtf8());
        EnvVar child("FAKE_PUBCMD_CHILD_PIDFILE", pidFile.toUtf8());
        const QString game = dir(QStringLiteral("pause/game"));
        const QString out = m_root.filePath(QStringLiteral("pause/out"));
        QVERIFY(makeSimpleDump(game));
        auto job = buildJob(game, out);
        CancellationSource source;
        qint64 before = -1, after = -1, childBefore = -1, childAfter = -1;
        std::thread pauser([&] {
            for (int i = 0; i < 400 && QFileInfo(ticks).size() < 5; ++i)
                QThread::msleep(25);
            source.pause();
            // The tool is suspended within 250 ms on an idle machine; wait
            // until neither counter has moved for half a second (a tool that
            // is never suspended runs to its last step, which fails below).
            qint64 seen = -1, childSeen = -1;
            for (int i = 0; i < 40; ++i) {
                QThread::msleep(500);
                const qint64 now = QFileInfo(ticks).size(), childNow = QFileInfo(childTicks).size();
                const bool still = now == seen && childNow == childSeen;
                seen = now;
                childSeen = childNow;
                if (still)
                    break;
            }
            before = QFileInfo(ticks).size();
            childBefore = QFileInfo(childTicks).size();
            QThread::msleep(1000);
            after = QFileInfo(ticks).size();
            childAfter = QFileInfo(childTicks).size();
            source.resume();
        });
        auto rr = run(job, {}, source.token());
        pauser.join();
        QVERIFY2(rr.result.ok(), qPrintable(rr.result.ok() ? QString() : rr.result.error().toLogString()));
        QVERIFY(before >= 5);
        QVERIFY2(before < kBarSteps, "the tool finished before it was paused");
        QCOMPARE(after, before);
#ifdef Q_OS_WIN
        QVERIFY2(childBefore > 0, "the stand-in did not start its child");
        QCOMPARE(childAfter, childBefore);
#endif
        QVERIFY(logged(QStringLiteral("Paused prospero-pub-cmd.exe.")));
        QVERIFY(logged(QStringLiteral("Resumed prospero-pub-cmd.exe.")));
        QVERIFY(checkPackageHeader(rr.result->outputs.value(0)).ok);
    }

    // Between the tool runs a paused job waits at the next phase boundary.
    void pausedJobWaitsBeforeTheNextPhase()
    {
        const QString game = dir(QStringLiteral("pausewait/game"));
        const QString out = m_root.filePath(QStringLiteral("pausewait/out"));
        QVERIFY(makeSimpleDump(game));
        auto job = buildJob(game, out);
        CancellationSource source;
        source.pause();
        bool ranWhilePaused = true;
        std::thread resumer([&] {
            QThread::msleep(700);
            ranWhilePaused = !invocations().isEmpty();
            source.resume();
        });
        auto rr = run(job, {}, source.token());
        resumer.join();
        QVERIFY(rr.result.ok());
        QVERIFY2(!ranWhilePaused, "img_create started while the job was paused");
    }

    void updateBuildWithBaseFolder()
    {
        const QString base = dir(QStringLiteral("upd/base-game"));
        QVERIFY(makeSimpleDump(base));
        put(base, QStringLiteral("Data/base_only.pak"), QByteArray(300, 'B'));
        const QString basePkg = m_root.filePath(QStringLiteral("upd/base/base.pkg"));
        QVERIFY(put(m_root.filePath(QStringLiteral("upd/base")), QStringLiteral("base.pkg"), fullPackageBytes()));
        const QString update = dir(QStringLiteral("upd/patch"));
        put(update, QStringLiteral("sce_sys/param.json"),
            QStringLiteral("{\"contentId\":\"%1\",\"contentVersion\":\"01.001.000\",\"applicationCategoryType\":0}").arg(kCid).toUtf8());
        put(update, QStringLiteral("Data/A.pak"), QByteArray(10, 'u'));
        const QString out = m_root.filePath(QStringLiteral("upd/out"));
        const QString stem = kCid + QStringLiteral("-A0101-V0101");
        QJsonObject p{{QStringLiteral("basePackage"), basePkg}, {QStringLiteral("baseFolder"), base}};
        auto job = buildJob(update, out, p);
        auto rr = run(job, {{keys::kKeepIntermediate, true}});
        QVERIFY2(rr.result.ok(), qPrintable(rr.result.ok() ? QString() : rr.result.error().toLogString()));
        QCOMPARE(rr.result->outputs, (QStringList{QDir(out).filePath(QStringLiteral("UPDATE_") + stem + QStringLiteral(".pkg")),
                                                  QDir(out).filePath(stem + QStringLiteral(".remastered.pkg"))}));
        QVERIFY(!checkPackageHeader(rr.result->outputs[0]).hasFih); // the delta
        QVERIFY(checkPackageHeader(rr.result->outputs[1]).ok);
        QVERIFY(!invocations().isEmpty());
        const QJsonArray argv = invocations().first().value(QStringLiteral("argv")).toArray();
        QVERIFY(argv.contains(QStringLiteral("--ref_pkg_path")));
        // The project merges both trees: update data wins, missing files come from the base.
        auto project = readGp5(QDir(out).filePath(stem + QStringLiteral(".gp5")));
        QVERIFY(project.ok());
        QHash<QString, QString> src;
        for (const auto& f : project->files)
            src.insert(f.dst, QDir::cleanPath(f.src));
        QCOMPARE(src.value(QStringLiteral("Data/A.pak")), QDir::cleanPath(QDir(update).filePath(QStringLiteral("Data/A.pak"))));
        QCOMPARE(src.value(QStringLiteral("Data/base_only.pak")), QDir::cleanPath(QDir(base).filePath(QStringLiteral("Data/base_only.pak"))));
        QCOMPARE(src.value(QStringLiteral("eboot.bin")), QDir::cleanPath(QDir(base).filePath(QStringLiteral("eboot.bin"))));
        QVERIFY(src.contains(QStringLiteral("sce_sys/keystone")));
        // Updates keep their size class.
        QVERIFY(!readAll(src.value(QStringLiteral("sce_sys/param.json"))).contains("attributePub"));

        // Output selection.
        auto overwritten = run(job, {{keys::kOutputConflict, QStringLiteral("overwrite")}});
        QVERIFY(overwritten.result.ok());
        QJsonObject pu = p;
        pu.insert(QStringLiteral("updateOutput"), QStringLiteral("update"));
        auto jobU = buildJob(update, m_root.filePath(QStringLiteral("upd/out-u")), pu);
        auto ru = run(jobU);
        QVERIFY(ru.result.ok());
        QCOMPARE(filesIn(m_root.filePath(QStringLiteral("upd/out-u"))).filter(QStringLiteral(".pkg")),
                 QStringList{QStringLiteral("UPDATE_") + stem + QStringLiteral(".pkg")});
        pu.insert(QStringLiteral("updateOutput"), QStringLiteral("full"));
        auto jobF = buildJob(update, m_root.filePath(QStringLiteral("upd/out-f")), pu);
        auto rf = run(jobF);
        QVERIFY(rf.result.ok());
        QCOMPARE(filesIn(m_root.filePath(QStringLiteral("upd/out-f"))).filter(QStringLiteral(".pkg")),
                 QStringList{stem + QStringLiteral(".remastered.pkg")});

        // Version not higher, other Content ID, nothing to update, bad base packages.
        put(update, QStringLiteral("sce_sys/param.json"),
            QStringLiteral("{\"contentId\":\"%1\",\"contentVersion\":\"01.000.000\",\"applicationCategoryType\":0}").arg(kCid).toUtf8());
        auto lower = run(job);
        QCOMPARE(errorOf(lower.result).code, ErrorCode::InvalidArgument);
        QVERIFY(errorOf(lower.result).message.contains(QStringLiteral("01.001.000"))); // suggested version
        put(update, QStringLiteral("sce_sys/param.json"),
            QStringLiteral("{\"contentId\":\"UP0000-PPSA01234_00-OTHERGAME0000000\",\"contentVersion\":\"01.001.000\",\"applicationCategoryType\":0}").toUtf8());
        QCOMPARE(errorOf(run(job).result).code, ErrorCode::InvalidArgument);
        const QString empty = dir(QStringLiteral("upd/empty"));
        auto nothing = buildJob(empty, m_root.filePath(QStringLiteral("upd/out-n")), p);
        auto rn = run(nothing);
        QCOMPARE(errorOf(rn.result).code, ErrorCode::InvalidArgument);
        QVERIFY(errorOf(rn.result).message.contains(QStringLiteral("nothing")));
        put(m_root.filePath(QStringLiteral("upd/base")), QStringLiteral("x.remastered.pkg"), fullPackageBytes());
        QJsonObject pr{{QStringLiteral("basePackage"), m_root.filePath(QStringLiteral("upd/base/x.remastered.pkg"))}};
        auto remastered = buildJob(update, out, pr);
        QCOMPARE(errorOf(run(remastered).result).code, ErrorCode::InvalidArgument);
        put(m_root.filePath(QStringLiteral("upd/base")), QStringLiteral("delta.pkg"), QByteArray(5000, 'D'));
        QJsonObject pd{{QStringLiteral("basePackage"), m_root.filePath(QStringLiteral("upd/base/delta.pkg"))},
                       {QStringLiteral("baseFolder"), base}};
        auto delta = buildJob(update, out, pd);
        QVERIFY(errorOf(run(delta).result).message.contains(QStringLiteral("not a full PS5 package")));
    }

    void updateExtractsBaseOrUsesExactSource()
    {
        EnvVar param("FAKE_PUBCMD_EXTRACT_PARAM",
                     QStringLiteral("{\"contentId\":\"%1\",\"contentVersion\":\"01.000.000\",\"applicationCategoryType\":0}").arg(kCid).toUtf8());
        const QString basePkg = m_root.filePath(QStringLiteral("updx/base.pkg"));
        put(m_root.filePath(QStringLiteral("updx")), QStringLiteral("base.pkg"), fullPackageBytes());
        const QString update = dir(QStringLiteral("updx/patch"));
        put(update, QStringLiteral("Data/shared.pak"), QByteArray(10, 'u'));
        const QString out = m_root.filePath(QStringLiteral("updx/out"));
        QJsonObject p{{QStringLiteral("basePackage"), basePkg}, {QStringLiteral("contentVersion"), QStringLiteral("01.002.000")}};
        auto job = buildJob(update, out, p);
        auto rr = run(job, {{keys::kKeepIntermediate, true}});
        QVERIFY2(rr.result.ok(), qPrintable(rr.result.ok() ? QString() : rr.result.error().toLogString()));
        const auto inv = invocations();
        QVERIFY(!inv.isEmpty());
        // The base package itself is what the tool is asked to extract, with the settings' passcode.
        const QStringList extractArgs = argvOf(inv.first());
        QCOMPARE(extractArgs.size(), 6);
        QCOMPARE(extractArgs.mid(0, 5), (QStringList{QStringLiteral("img_extract"), QStringLiteral("--passcode"), kDefaultPasscode,
                                                     QStringLiteral("--no_progress_bar"), QDir::toNativeSeparators(basePkg)}));
        const QString stem = kCid + QStringLiteral("-A0102-V0102");
        const QByteArray gp5 = readAll(QDir(out).filePath(stem + QStringLiteral(".gp5")));
        QVERIFY(gp5.contains("dst_path=\"Data/base_only.pak\""));
        QVERIFY(gp5.contains("patch-base"));
        QVERIFY(!gp5.contains("patch-base/playgo-chunks") && !gp5.contains("patch-base\\playgo-chunks"));
        QVERIFY(gp5.contains("dst_path=\"sce_sys/param.json\""));
        // No temporary folder is set: the base was extracted next to the output, in a work
        // folder that is gone when the build ends.
        const QString extractedTo = extractArgs.last();
        QVERIFY2(extractedTo.contains(QStringLiteral(".fw-stage-")), qPrintable(extractedTo));
        QVERIFY(QDir(out).entryList({QStringLiteral(".fw-stage-*")}, QDir::AllEntries | QDir::Hidden).isEmpty());
        QVERIFY(readAll(QDir(out).filePath(stem + QStringLiteral("-build-logs/01-create-gp5.log"))).contains("temporary paths"));

        // Exact source: no extraction, the base is not checked (warning).
        QFile::remove(m_fakeLog);
        put(update, QStringLiteral("sce_sys/param.json"),
            QStringLiteral("{\"contentId\":\"%1\",\"contentVersion\":\"01.003.000\",\"applicationCategoryType\":0}").arg(kCid).toUtf8());
        put(update, QStringLiteral("sce_sys/keystone"), QByteArray(96, 'K'));
        QJsonObject pe{{QStringLiteral("basePackage"), basePkg}, {QStringLiteral("exactSource"), true}};
        auto exact = buildJob(update, out, pe);
        auto re = run(exact);
        QVERIFY2(re.result.ok(), qPrintable(re.result.ok() ? QString() : re.result.error().toLogString()));
        QVERIFY(!invocations().isEmpty());
        for (const auto& i : invocations())
            QVERIFY(i.value(QStringLiteral("argv")).toArray().at(0).toString() != u"img_extract");
        QVERIFY(std::any_of(re.result->warnings.begin(), re.result->warnings.end(),
                            [](const QString& w) { return w.contains(QStringLiteral("not checked")); }));
    }

    void addonBuild()
    {
        const QString addon = dir(QStringLiteral("addon/addcont0"));
        put(addon, QStringLiteral("sce_sys/param.json"),
            QStringLiteral("{\"contentId\":\"%1\",\"contentVersion\":\"01.000.000\"}").arg(kAddonCid).toUtf8());
        put(addon, QStringLiteral("data.bin"), QByteArray(300, 'd'));
        const QString out = m_root.filePath(QStringLiteral("addon/out"));
        auto make = [&](const QString& key, const QString& volume, const QString& folder) {
            return FpkgAddonJob(requestFrom<AddonRequest>({{QStringLiteral("payloadFolder"), folder},
                                                           {QStringLiteral("output"), out},
                                                           {QStringLiteral("entitlementKey"), key},
                                                           {QStringLiteral("volumeType"), volume}}));
        };
        auto job = make(kKey, QStringLiteral("prospero_ac"), addon);
        auto rr = run(job, {{keys::kKeepIntermediate, true}});
        QVERIFY2(rr.result.ok(), qPrintable(rr.result.ok() ? QString() : rr.result.error().toLogString()));
        QCOMPARE(QFileInfo(rr.result->outputs.first()).fileName(), kAddonCid + QStringLiteral("-A0100-V0100.pkg"));
        QVERIFY(checkPackageHeader(rr.result->outputs.first()).ok);
        auto project = readGp5(QDir(out).filePath(kAddonCid + QStringLiteral("-A0100-V0100.gp5")));
        QVERIFY(project.ok());
        QCOMPARE(project->volumeType, QStringLiteral("prospero_ac"));
        QCOMPARE(project->entitlementKey, kKey.toLower());
        QVERIFY(!project->hasChunkInfo);
        // "Keep both": the first package is moved aside under the next free name.
        QVERIFY(run(job, {}).result.ok());
        QVERIFY(checkPackageHeader(QDir(out).filePath(kAddonCid + QStringLiteral("-A0100-V0100.pkg"))).ok);
        QVERIFY(checkPackageHeader(QDir(out).filePath(kAddonCid + QStringLiteral("-A0100-V0100 (1).pkg"))).ok);
        auto al = make(kKey, QStringLiteral("prospero_al"), addon);
        QCOMPARE(errorOf(run(al).result).code, ErrorCode::Unsupported);
        auto badKey = make(QStringLiteral("xyz"), QStringLiteral("prospero_ac"), addon);
        QCOMPARE(errorOf(run(badKey).result).code, ErrorCode::InvalidArgument);
        const QString noParam = dir(QStringLiteral("addon/addcont1"));
        put(noParam, QStringLiteral("data.bin"), "x");
        auto np = make(kKey, QStringLiteral("prospero_ac"), noParam);
        QVERIFY(errorOf(run(np).result).message.contains(QStringLiteral("param.json")));
        // dlc_emu.ini candidates for the add-on dialog.
        const QString gameDir = m_root.filePath(QStringLiteral("addon"));
        put(gameDir, QStringLiteral("dlc_emu.ini"),
            QStringLiteral("[PSAC]\ncontent_id=%1\nmount_point=/app0/addcont0\nentitlement_key=%2\n[PSAL]\ncontent_id=%3\n"
                           "[PSAC]\ncontent_id=UP0000-PPSA01234_00-DLCPACK000000009\nmount_point=/app0/addcont9\n")
                .arg(kAddonCid, kKey, QStringLiteral("UP0000-PPSA01234_00-DLCPACK000000002")).toUtf8());
        const auto cands = addonsFromDlcEmu(gameDir);
        QCOMPARE(int(cands.size()), 3);
        QVERIFY(cands[0].supported);
        QCOMPARE(cands[0].entitlementKey, kKey);
        QVERIFY(!cands[1].supported && cands[1].reason.contains(QStringLiteral("installer")));
        QVERIFY(!cands[2].supported);
    }

    void verifyAndExtractJobs()
    {
        const QString game = dir(QStringLiteral("vx/game"));
        const QString out = m_root.filePath(QStringLiteral("vx/out"));
        QVERIFY(makeSimpleDump(game));
        auto build = buildJob(game, out);
        QVERIFY(run(build).result.ok());
        const QString pkg = QDir(out).filePath(kStem + QStringLiteral(".pkg"));

        FpkgVerifyJob verify(requestFrom<VerifyRequest>({{QStringLiteral("package"), pkg}}));
        QFile::remove(m_fakeLog);
        auto v = run(verify);
        QVERIFY(v.result.ok());
        QCOMPARE(v.result->verification, Verification::Passed);
        {
            const auto inv = invocations();
            QCOMPARE(inv.size(), 1);
            QCOMPARE(argvOf(inv[0]), (QStringList{QStringLiteral("img_verify"), QStringLiteral("--passcode"), kDefaultPasscode,
                                                  QStringLiteral("--format_check"), QStringLiteral("on"),
                                                  QStringLiteral("--integrity_check"), QStringLiteral("on"),
                                                  QStringLiteral("--no_progress_bar"), QDir::toNativeSeparators(pkg)}));
        }
        // The package was built with the settings' passcode: another one does not open it.
        const QString otherPasscode = QStringLiteral("0123456789abcdef0123456789abcdef");
        {
            FpkgVerifyJob wrong(requestFrom<VerifyRequest>({{QStringLiteral("package"), pkg}, {QStringLiteral("passcode"), otherPasscode}}));
            auto vw = run(wrong);
            QVERIFY(vw.result.ok());
            QCOMPARE(vw.result->verification, Verification::Failed);
        }
        {
            EnvVar fail("FAKE_PUBCMD_VERIFY", "fail");
            auto vf = run(verify);
            QVERIFY(vf.result.ok());
            QCOMPARE(vf.result->verification, Verification::Failed); // the queue reports it as failed
        }
        // An update delta is verified through its .remastered.pkg.
        put(out, QStringLiteral("UPDATE_X-A0100-V0100.pkg"), QByteArray(5000, 'D'));
        put(out, QStringLiteral("X-A0100-V0100.remastered.pkg"), fullPackageBytes());
        FpkgVerifyJob verifyDelta(requestFrom<VerifyRequest>({{QStringLiteral("package"), QDir(out).filePath(QStringLiteral("UPDATE_X-A0100-V0100.pkg"))}}));
        auto vd = run(verifyDelta);
        QVERIFY(vd.result.ok());
        QCOMPARE(vd.result->verification, Verification::Passed);
        QVERIFY(vd.result->outputs.first().endsWith(QStringLiteral(".remastered.pkg")));

        const QString dest = m_root.filePath(QStringLiteral("vx/extracted"));
        FpkgExtractJob extract(requestFrom<ExtractRequest>({{QStringLiteral("package"), pkg}, {QStringLiteral("destination"), dest}}));
        QFile::remove(m_fakeLog);
        auto x = run(extract);
        QVERIFY(x.result.ok());
        {
            const auto inv = invocations();
            QCOMPARE(inv.size(), 1);
            QCOMPARE(argvOf(inv[0]), (QStringList{QStringLiteral("img_extract"), QStringLiteral("--passcode"), kDefaultPasscode,
                                                  QStringLiteral("--no_progress_bar"), QDir::toNativeSeparators(pkg),
                                                  QDir::toNativeSeparators(dest)}));
        }
        QVERIFY(QFileInfo::exists(QDir(dest).filePath(QStringLiteral("eboot.bin"))));
        // The tool was given this package: its own param.json came out.
        QVERIFY(readAll(QDir(dest).filePath(QStringLiteral("sce_sys/param.json"))).contains(kCid.toUtf8()));
        QVERIFY(x.result->summary.contains(QStringLiteral("files")));
        QCOMPARE(errorOf(run(extract).result).code, ErrorCode::AlreadyExists); // never into a non-empty folder
        {
            // A wrong passcode: the tool refuses, and the folder made for it is removed.
            const QString destW = m_root.filePath(QStringLiteral("vx/extracted-wrong"));
            FpkgExtractJob xw(requestFrom<ExtractRequest>({{QStringLiteral("package"), pkg}, {QStringLiteral("destination"), destW},
                                                           {QStringLiteral("passcode"), otherPasscode}}));
            QCOMPARE(errorOf(run(xw).result).code, ErrorCode::ExternalToolFailed);
            QVERIFY(!QFileInfo::exists(destW));
        }
        {
            // The tool fails after it wrote its first file.
            EnvVar fail("FAKE_PUBCMD_EXTRACT", "fail");
            const QString dest2 = m_root.filePath(QStringLiteral("vx/extracted2"));
            FpkgExtractJob x2(requestFrom<ExtractRequest>({{QStringLiteral("package"), pkg}, {QStringLiteral("destination"), dest2}}));
            QCOMPARE(errorOf(run(x2).result).code, ErrorCode::ExternalToolFailed);
            QVERIFY(!QFileInfo::exists(dest2)); // created by the job, removed again with what was in it
        }
        {
            EnvVar slow("FAKE_PUBCMD_EXTRACT", "slow");
            const QString dest3 = dir(QStringLiteral("vx/extracted3")); // exists and is empty
            FpkgExtractJob x3(requestFrom<ExtractRequest>({{QStringLiteral("package"), pkg}, {QStringLiteral("destination"), dest3}}));
            CancellationSource cancel;
            // Cancel once the tool has written its files (it then idles for 10 s).
            std::atomic<bool> sawOutput{false}, finished{false};
            std::thread t([&] {
                const QString last = QDir(dest3).filePath(QStringLiteral("playgo-chunks/001.bin"));
                while (!sawOutput && !finished) {
                    if (QFileInfo::exists(last))
                        sawOutput = true;
                    else
                        QThread::msleep(25);
                }
                cancel.cancel();
            });
            auto r3 = run(x3, {}, cancel.token());
            finished = true;
            t.join();
            QVERIFY2(sawOutput, "the tool had written nothing when the job ended");
            QVERIFY(errorOf(r3.result).isCancelled());
            QVERIFY(QFileInfo(dest3).isDir());
            QVERIFY(QDir(dest3).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)); // partial output removed
        }
    }

    void nonAsciiFoldersGoThroughLinks()
    {
        if (!isAsciiPath(m_root.path()))
            QSKIP("The temporary folder itself has a non-ASCII path: the links would be made outside it.");
        const QString game = dir(QStringLiteral("Yōtei ゲーム"));
        const QString out = m_root.filePath(QStringLiteral("Ausgabe é"));
        QVERIFY(makeSimpleDump(game));
        auto job = buildJob(game, out);
        auto rr = run(job);
        QVERIFY2(rr.result.ok(), qPrintable(rr.result.ok() ? QString() : rr.result.error().toLogString()));
        QCOMPARE(QFileInfo(rr.result->outputs.first()).absolutePath(), QFileInfo(out).absoluteFilePath());
        QVERIFY(checkPackageHeader(rr.result->outputs.first()).ok);
        QCOMPARE(invocations().size(), 2);
        for (const auto& inv : invocations()) {
            for (const auto& a : inv.value(QStringLiteral("argv")).toArray())
                QVERIFY2(isAsciiPath(a.toString()), qPrintable(a.toString()));
            QVERIFY(isAsciiPath(inv.value(QStringLiteral("TEMP")).toString()));
        }
        // The links were made in the folder for them under this test's temporary folder ...
        const QString linkFolder = QDir::toNativeSeparators(QDir(QDir::tempPath()).filePath(AsciiAliases::aliasFolderName()));
        QVERIFY(QFileInfo(linkFolder).isDir());
        QVERIFY2(logged(QStringLiteral("Using the ASCII link ") + linkFolder), qPrintable(linkFolder));
        // ... and every one was removed; the real folders are untouched.
        QCOMPARE(aliasLinksLeft(), QStringList());
        QVERIFY(QFileInfo::exists(QDir(game).filePath(QStringLiteral("eboot.bin"))));
        // A name inside the game that the tools reject stops the build before packing.
        put(game, QStringLiteral("Data/café.pak"), "x");
        QFile::remove(m_fakeLog);
        auto bad = run(job);
        QCOMPARE(errorOf(bad.result).code, ErrorCode::Unsupported);
        QVERIFY(errorOf(bad.result).message.contains(QStringLiteral("non-ASCII")));
        QVERIFY(invocations().isEmpty());
    }

    void gp5ProjectSource()
    {
        const QString game = dir(QStringLiteral("gp5src/game"));
        QVERIFY(makeSimpleDump(game));
        Gp5Project g;
        g.passcode = kDefaultPasscode;
        g.contentId = kCid;
        g.chunkInfo = Gp5ChunkInfo{{{QStringLiteral("chunk_count"), QStringLiteral("1")}, {QStringLiteral("scenario_count"), QStringLiteral("1")}},
                                   {},
                                   {{{{QStringLiteral("id"), QStringLiteral("0")}, {QStringLiteral("label"), QStringLiteral("Chunk #0")}}}},
                                   {{QStringLiteral("default_id"), QStringLiteral("0")}},
                                   {{{{QStringLiteral("id"), QStringLiteral("0")}, {QStringLiteral("type"), QStringLiteral("playmode")},
                                      {QStringLiteral("initial_chunk_count"), QStringLiteral("1")}, {QStringLiteral("label"), QStringLiteral("S")}},
                                     QStringLiteral("0")}}};
        for (const char* rel : {"eboot.bin", "sce_sys/param.json", "sce_sys/keystone", "Data/A.pak"})
            g.files.push_back({QString::fromLatin1(rel), QString::fromLatin1(rel), std::nullopt, false});
        // Relative paths: the tool runs in the project's folder.
        const QString project = QDir(game).filePath(QStringLiteral("my.gp5"));
        QVERIFY(put(game, QStringLiteral("my.gp5"), writeGp5(g)));
        const QString out = m_root.filePath(QStringLiteral("gp5src/out"));
        auto job = buildJob(project, out);
        auto rr = run(job);
        QVERIFY2(rr.result.ok(), qPrintable(rr.result.ok() ? QString() : rr.result.error().toLogString()));
        QCOMPARE(QFileInfo(rr.result->outputs.first()).fileName(), kStem + QStringLiteral(".pkg"));
        QVERIFY(QFileInfo::exists(project)); // the user's project is never removed
        QVERIFY(!invocations().isEmpty());
        QCOMPARE(QDir::cleanPath(invocations().first().value(QStringLiteral("cwd")).toString()), QDir::cleanPath(QFileInfo(game).canonicalFilePath()));
        // img_verify gets the project's own passcode, not the one from the settings.
        QFile::remove(m_fakeLog);
        g.passcode = QStringLiteral("0123456789abcdef0123456789abcdef");
        put(game, QStringLiteral("my.gp5"), writeGp5(g));
        QVERIFY(run(job).result.ok());
        {
            const auto inv = invocations();
            QCOMPARE(inv.size(), 2);
            const QJsonArray argv = inv[1].value(QStringLiteral("argv")).toArray();
            QCOMPARE(argv.at(0).toString(), QStringLiteral("img_verify"));
            QCOMPARE(argv.at(2).toString(), g.passcode);
        }
        g.passcode = kDefaultPasscode;
        put(game, QStringLiteral("my.gp5"), writeGp5(g));
        // "Stop with an error" refuses an existing package before packing, as for a folder.
        QFile::remove(m_fakeLog);
        QCOMPARE(errorOf(run(job, {{keys::kOutputConflict, QStringLiteral("fail")}}).result).code, ErrorCode::AlreadyExists);
        QVERIFY(invocations().isEmpty());
        // Missing files and non-ASCII source paths are refused before packing.
        g.files.push_back({QStringLiteral("Data/missing.pak"), QStringLiteral("Data/missing.pak"), std::nullopt, false});
        put(game, QStringLiteral("my.gp5"), writeGp5(g));
        QCOMPARE(errorOf(run(job).result).code, ErrorCode::NotFound);
        g.files.pop_back();
        put(game, QStringLiteral("Data/ü.pak"), "x");
        g.files.push_back({QStringLiteral("Data/u.pak"), QStringLiteral("Data/ü.pak"), std::nullopt, false});
        put(game, QStringLiteral("my.gp5"), writeGp5(g));
        QCOMPARE(errorOf(run(job).result).code, ErrorCode::Unsupported);
        // Version or title changes cannot apply to a user's project.
        auto changed = buildJob(project, out, {{QStringLiteral("contentVersion"), QStringLiteral("01.001.000")}});
        QCOMPARE(errorOf(run(changed).result).code, ErrorCode::InvalidArgument);
    }

    void imageSourceThroughStager()
    {
        const QString tree = dir(QStringLiteral("img/tree"));
        QVERIFY(makeSimpleDump(QDir(tree).filePath(QStringLiteral("PPSA01234-app"))));
        const QString image = m_root.filePath(QStringLiteral("img/game.exfat"));
        put(m_root.filePath(QStringLiteral("img")), QStringLiteral("game.exfat"), "not really an image");
        unregisterSourceStagers();
        auto job = buildJob(image, m_root.filePath(QStringLiteral("img/out")));
        QCOMPARE(errorOf(run(job).result).code, ErrorCode::Unsupported);
        auto stager = std::make_shared<FolderStager>(tree);
        registerSourceStager(stager);
        auto rr = run(job);
        unregisterSourceStagers();
        QVERIFY2(rr.result.ok(), qPrintable(rr.result.ok() ? QString() : rr.result.error().toLogString()));
        QCOMPARE(stager->calls, 1);
        QCOMPARE(QFileInfo(rr.result->outputs.first()).fileName(), kStem + QStringLiteral(".pkg"));
        QCOMPARE(QDir::cleanPath(QFileInfo(rr.result->outputs.first()).absolutePath()), QDir::cleanPath(m_root.filePath(QStringLiteral("img/out"))));
    }

    void imagesAndSelfRepairsInTheProject()
    {
        const QString game = dir(QStringLiteral("rep/game"));
        QVERIFY(makeSimpleDump(game));
        put(game, QStringLiteral("sce_sys/pic2.dds"), "DDS ");
        put(game, QStringLiteral("eboot.bin"), QByteArray("\x4F\x15\x3D\x1D", 4) + QByteArray(100, '\0'));
        const QString out = m_root.filePath(QStringLiteral("rep/out"));
        auto job = buildJob(game, out);
        auto rr = run(job, {{keys::kKeepIntermediate, true}});
        QVERIFY2(rr.result.ok(), qPrintable(rr.result.ok() ? QString() : rr.result.error().toLogString()));
        auto project = readGp5(QDir(out).filePath(kStem + QStringLiteral(".gp5")));
        QVERIFY(project.ok());
        QCOMPARE(project->files[1].dst, QStringLiteral("sce_sys/pic2.png")); // right after the scenario sidecar
        QCOMPARE(readPngInfo(project->files[1].src).colorType, 6);
        QString ebootSrc;
        for (const auto& f : project->files)
            if (f.dst == u"eboot.bin")
                ebootSrc = f.src;
        QVERIFY(ebootSrc.contains(QStringLiteral("normalized-self")));
        QCOMPARE(readAll(ebootSrc).left(4), QByteArray("\x54\x14\xF5\xEE", 4));
        QCOMPARE(readAll(QDir(game).filePath(QStringLiteral("eboot.bin"))).left(4), QByteArray("\x4F\x15\x3D\x1D", 4));

        EnvVar wrong("FAKE_DDS2PNG_MODE", "wrong");
        auto bad = run(job, {{keys::kOutputConflict, QStringLiteral("overwrite")}});
        QCOMPARE(errorOf(bad.result).code, ErrorCode::ExternalToolFailed);
        QVERIFY(errorOf(bad.result).message.contains(QStringLiteral("pic2.png")));
    }

    void sameContentIdNeverOverlaps()
    {
        EnvVar delay("FAKE_PUBCMD_DELAY_MS", "15");
        const QString game = dir(QStringLiteral("same/game"));
        const QString out = m_root.filePath(QStringLiteral("same/out"));
        QVERIFY(makeSimpleDump(game));
        auto a = buildJob(game, out);
        auto b = buildJob(game, out);
        RunResult ra, rb;
        std::thread ta([&] { ra = run(a, {{keys::kPostVerify, QStringLiteral("headerOnly")}}); });
        std::thread tb([&] { rb = run(b, {{keys::kPostVerify, QStringLiteral("headerOnly")}}); });
        ta.join();
        tb.join();
        QVERIFY(ra.result.ok());
        QVERIFY(rb.result.ok());
        const auto inv = invocations();
        QCOMPARE(inv.size(), 2);
        const qint64 a0 = qint64(inv[0].value(QStringLiteral("t0")).toDouble()), a1 = qint64(inv[0].value(QStringLiteral("t1")).toDouble());
        const qint64 b0 = qint64(inv[1].value(QStringLiteral("t0")).toDouble());
        QVERIFY2(b0 >= a1, qPrintable(QStringLiteral("%1-%2 then %3").arg(a0).arg(a1).arg(b0)));
        QVERIFY(QFileInfo::exists(QDir(out).filePath(kStem + QStringLiteral(" (1).pkg"))));
    }

    void jobFactoryRoundTripAndValidation()
    {
        const QString game = dir(QStringLiteral("factory/game"));
        QVERIFY(makeSimpleDump(game));
        const QJsonObject params{{QStringLiteral("source"), game},
                                 {QStringLiteral("output"), m_root.filePath(QStringLiteral("factory/out"))},
                                 {QStringLiteral("contentVersion"), QStringLiteral("01.000.000")}};
        auto job = m_env.jobs.create(QStringLiteral("fpkg.build"), params);
        QVERIFY(job.ok());
        QCOMPARE(job.value()->parameters(), params);
        QCOMPARE(job.value()->title(), QStringLiteral("Test Game (PPSA01234)"));
        QCOMPARE(job.value()->phases().size(), 9);
        QVERIFY(!m_env.jobs.create(QStringLiteral("fpkg.build"), {}).ok());
        QVERIFY(!m_env.jobs.create(QStringLiteral("fpkg.build"), {{QStringLiteral("source"), game}, {QStringLiteral("updateOutput"), QStringLiteral("x")}}).ok());
        QVERIFY(m_env.jobs.create(QStringLiteral("fpkg.verify"), {{QStringLiteral("package"), QStringLiteral("x.pkg")}}).ok());
        // validate(): no tools, missing source, output inside the source, bad override.
        QCOMPARE(errorOf(job.value()->validate(m_env.values())).code, ErrorCode::DependencyMissing);
        auto missing = buildJob(m_root.filePath(QStringLiteral("nope")), QString());
        QCOMPARE(errorOf(missing.validate(m_env.values(baseSettings()))).code, ErrorCode::NotFound);
        auto inside = buildJob(game, QDir(game).filePath(QStringLiteral("out")));
        QCOMPARE(errorOf(inside.validate(m_env.values(baseSettings()))).code, ErrorCode::InvalidArgument);
        auto badVer = buildJob(game, QString(), {{QStringLiteral("contentVersion"), QStringLiteral("1.0")}});
        QCOMPARE(errorOf(badVer.validate(m_env.values(baseSettings()))).code, ErrorCode::InvalidArgument);
        auto noSceSys = buildJob(dir(QStringLiteral("factory/plain")), QString());
        QCOMPARE(errorOf(noSceSys.validate(m_env.values(baseSettings()))).code, ErrorCode::NotFound);
    }
};

QTEST_GUILESS_MAIN(TstFpkgBuild)
#include "tst_fpkg_build.moc"
