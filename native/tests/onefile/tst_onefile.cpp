// Single-file launcher (src/onefile): packs a small payload around a stand-in
// program (fw_onefile_test_child, plus extra files in sub-folders) with
// fw_onefile_pack and runs the launcher the way users and scripts do: first
// start, cached start, arguments, exit code, standard streams, working folder,
// damaged payloads, concurrent first starts, old-version cleanup, the %TEMP%
// fallback, portable mode and a console without redirection.
//
// Every run uses private LOCALAPPDATA/TEMP folders, FW_ONEFILE_NO_UI=1 (no
// windows, errors on stderr) and FW_ONEFILE_DEBUG=1 (trace lines on stderr).

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <vector>

#ifndef FW_ONEFILE_STUB_PATH // the launcher exists on Windows only (src/onefile)

class TstOnefile : public QObject {
    Q_OBJECT
private slots:
    void windowsOnly() { QSKIP("The single-file launcher is Windows only"); }
};

#else

#include <windows.h>
#include <shellapi.h>

namespace {

const QString kEntry = QStringLiteral("exFAT Image Builder.exe");
const QString kRuntimeTail = QStringLiteral("kerrdec97/exFAT Image Builder/runtime");

struct Run {
    int exitCode = -1;
    QString out;
    QString err;

    QString value(const QString& key) const
    {
        for (const QString& line : out.split(QLatin1Char('\n')))
            if (line.startsWith(key + QLatin1Char('=')))
                return line.mid(key.size() + 1);
        return {};
    }
    QStringList args() const
    {
        QStringList a;
        for (const QString& line : out.split(QLatin1Char('\n')))
            if (line.startsWith(QLatin1String("arg=")))
                a << line.mid(4);
        return a;
    }
    bool childRan() const { return out.contains(QLatin1String("\nend\n")); }
    QString describe() const { return QStringLiteral("exit %1\n--- stdout\n%2\n--- stderr\n%3").arg(exitCode).arg(out, err); }
};

QString canonical(const QString& p)
{
    const QString c = QFileInfo(p).canonicalFilePath();
    return (c.isEmpty() ? QDir::cleanPath(QDir::fromNativeSeparators(p)) : c).toLower();
}

QByteArray sha256Of(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return h.result().toHex();
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

QStringList folders(const QString& dir)
{
    QStringList l = QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    return l;
}

} // namespace

class TstOnefile : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
    QString m_payload;
    QString m_dist;
    QString m_onefile;
    int m_fresh = 0;

    struct Dirs {
        QString local, temp;
        QString runtime() const { return local + QLatin1Char('/') + kRuntimeTail; }
    };

    Dirs fresh()
    {
        const QString base = m_tmp.filePath(QStringLiteral("env-%1").arg(++m_fresh));
        Dirs d{base + QStringLiteral("/Local AppData"), base + QStringLiteral("/Temp ü")};
        QDir().mkpath(d.local);
        QDir().mkpath(d.temp);
        return d;
    }

    static QProcessEnvironment envFor(const Dirs& d)
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("LOCALAPPDATA"), QDir::toNativeSeparators(d.local));
        env.insert(QStringLiteral("TEMP"), QDir::toNativeSeparators(d.temp));
        env.insert(QStringLiteral("TMP"), QDir::toNativeSeparators(d.temp));
        env.insert(QStringLiteral("FW_ONEFILE_NO_UI"), QStringLiteral("1"));
        env.insert(QStringLiteral("FW_ONEFILE_DEBUG"), QStringLiteral("1"));
        env.remove(QStringLiteral("FW_ONEFILE_NO_MUTEX"));
        env.remove(QStringLiteral("FW_LAUNCHER_DIR"));
        env.remove(QStringLiteral("FW_LAUNCHER_EXE"));
        return env;
    }

    static void setup(QProcess& p, const QString& exe, const QStringList& args, const QProcessEnvironment& env,
                      const QString& cwd, const QString& native)
    {
        p.setProgram(exe);
        p.setArguments(args);
        if (!native.isNull())
            p.setNativeArguments(native);
        p.setProcessEnvironment(env);
        if (!cwd.isEmpty())
            p.setWorkingDirectory(cwd);
    }

    static Run finish(QProcess& p)
    {
        Run r;
        if (!p.waitForFinished(120000)) {
            p.kill();
            p.waitForFinished();
            r.err = QStringLiteral("timeout");
            return r;
        }
        r.exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
        r.out = QString::fromUtf8(p.readAllStandardOutput()).remove(QLatin1Char('\r'));
        r.err = QString::fromUtf8(p.readAllStandardError()).remove(QLatin1Char('\r'));
        if (!r.out.isEmpty() && !r.out.startsWith(QLatin1Char('\n')))
            r.out.prepend(QLatin1Char('\n'));
        return r;
    }

    static Run run(const QString& exe, const QStringList& args, const QProcessEnvironment& env, const QString& cwd = {},
                   const QString& native = QString())
    {
        QProcess p;
        setup(p, exe, args, env, cwd, native);
        p.start();
        if (!p.waitForStarted(30000))
            return Run{-2, {}, p.errorString()};
        return finish(p);
    }

    static Run pack(const QStringList& args)
    {
        return run(QStringLiteral(FW_ONEFILE_PACK_PATH), args, QProcessEnvironment::systemEnvironment());
    }

    bool packTo(const QString& output, const QString& version, const QString& stub = QStringLiteral(FW_ONEFILE_STUB_PATH))
    {
        QDir().mkpath(QFileInfo(output).absolutePath());
        const Run r = pack({QStringLiteral("pack"), QStringLiteral("--stub"), stub, QStringLiteral("--input"), m_payload,
                            QStringLiteral("--entry"), kEntry, QStringLiteral("--output"), output, QStringLiteral("--version"),
                            version});
        if (r.exitCode != 0)
            qWarning().noquote() << r.describe();
        return r.exitCode == 0;
    }

    // The single complete runtime folder under `root`, or an empty string
    // when there is none or more than one (the caller asserts on that).
    static QString onlyRuntime(const QString& root)
    {
        const QStringList l = folders(root);
        return l.size() == 1 ? root + QLatin1Char('/') + l.first() : QString();
    }

    // cmd.exe-style start without redirection: the launcher runs inside a
    // console of its own and what that console shows comes back as `out`.
    Run runInConsole(const QStringList& args)
    {
        QProcess p;
        p.setProgram(QStringLiteral(FW_ONEFILE_CONSOLE_PATH));
        p.setArguments(QStringList{QDir::toNativeSeparators(m_onefile)} + args);
        p.setProcessEnvironment(envFor(fresh()));
        p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) { a->flags |= CREATE_NO_WINDOW; });
        p.start();
        if (!p.waitForStarted(30000))
            return Run{-2, {}, p.errorString()};
        return finish(p);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        m_payload = m_tmp.filePath(QStringLiteral("payload"));
        QVERIFY(QDir().mkpath(m_payload));
        QVERIFY(QFile::copy(QStringLiteral(FW_ONEFILE_CHILD_PATH), m_payload + QLatin1Char('/') + kEntry));
        // Incompressible data over two chunks (stored), compressible text, a
        // non-ASCII name and an empty file.
        QByteArray random(9 * 1024 * 1024 + 12345, Qt::Uninitialized);
        QRandomGenerator rng(1234);
        for (char& c : random)
            c = char(rng.generate() & 0xFF);
        QVERIFY(writeFile(m_payload + QStringLiteral("/engines/backport/big.bin"), random));
        QByteArray text;
        for (int i = 0; text.size() < 3 * 1024 * 1024; ++i)
            text += "line " + QByteArray::number(i) + " of a very compressible plugin file\n";
        QVERIFY(writeFile(m_payload + QStringLiteral("/plugins/platforms/qtext.dll"), text));
        QVERIFY(writeFile(m_payload + QString::fromUtf8("/LICENSES/ünïcødé ✓ 日本.txt"), "licence text\n"));
        QVERIFY(writeFile(m_payload + QStringLiteral("/empty.txt"), QByteArray()));

        m_dist = m_tmp.filePath(QStringLiteral("dist"));
        m_onefile = m_dist + QLatin1Char('/') + kEntry;
        QVERIFY(packTo(m_onefile, QStringLiteral("1.0.0")));
    }

    void firstStartUnpacksEveryFileAndRunsTheProgram()
    {
        const Dirs d = fresh();
        const Run r = run(m_onefile, {}, envFor(d));
        QVERIFY2(r.exitCode == 0 && r.childRan(), qPrintable(r.describe()));
        QVERIFY2(r.err.contains(QLatin1String("fw-onefile: unpacked 5 files")), qPrintable(r.err));

        const QStringList all = folders(d.runtime());
        QCOMPARE(all.size(), 1); // no .tmp- or .trash- leftovers
        QVERIFY2(QRegularExpression(QStringLiteral("^1\\.0\\.0-[0-9a-f]{12}$")).match(all.first()).hasMatch(), qPrintable(all.first()));
        const QString dir = d.runtime() + QLatin1Char('/') + all.first();
        QVERIFY(QFileInfo::exists(dir + QStringLiteral("/.onefile-complete")));
        QDirIterator it(m_payload, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        int files = 0;
        while (it.hasNext()) {
            const QString src = it.next();
            const QString rel = QDir(m_payload).relativeFilePath(src);
            QVERIFY2(QFileInfo::exists(dir + QLatin1Char('/') + rel), qPrintable(rel));
            QCOMPARE(sha256Of(dir + QLatin1Char('/') + rel), sha256Of(src));
            ++files;
        }
        QCOMPARE(files, 5);
        QCOMPARE(canonical(r.value(QStringLiteral("exe"))), canonical(dir + QLatin1Char('/') + kEntry));
    }

    void argumentsPassThrough()
    {
        const QStringList args = {QStringLiteral("plain"),
                                  QStringLiteral("with space"),
                                  QStringLiteral("quote\"inside"),
                                  QStringLiteral("trailing\\"),
                                  QStringLiteral("back\\\\\"slash quote"),
                                  QString::fromUtf8("ünïcødé 日本語 ✓"),
                                  QString(),
                                  QStringLiteral("--flag=a b"),
                                  QStringLiteral("C:\\Some Folder\\")};
        const Run r = run(m_onefile, args, envFor(fresh()));
        QVERIFY2(r.exitCode == 0, qPrintable(r.describe()));
        QCOMPARE(r.args(), args);
    }

    void rawCommandLinePassThrough_data()
    {
        QTest::addColumn<QString>("tail");
        QTest::newRow("words") << QStringLiteral("a b c");
        QTest::newRow("quotes") << QStringLiteral("\"a b\" \"c\\\"d\" e");
        QTest::newRow("backslashes") << QStringLiteral("C:\\path\\ \"C:\\dir with space\\\\\" x\\\\\\\"y z");
        QTest::newRow("spacing") << QStringLiteral("   lots   of\t spaces  ");
        QTest::newRow("unicode") << QString::fromUtf8("\"ünï cødé\" 日本語 ✓");
        QTest::newRow("empty args") << QStringLiteral("\"\" a \"\"");
        QTest::newRow("open quote") << QStringLiteral("\"never closed quote");
        QTest::newRow("doubled quotes") << QStringLiteral("\"a\"\"b\" c\"\"\"d");
    }

    void rawCommandLinePassThrough()
    {
        QFETCH(QString, tail);
        // What the program would see if started directly with this command line.
        const std::wstring line = L"\"x.exe\" " + tail.toStdWString();
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(line.c_str(), &argc);
        QStringList expected;
        for (int i = 1; i < argc; ++i)
            expected << QString::fromWCharArray(argv[i]);
        LocalFree(argv);

        const Run r = run(m_onefile, {}, envFor(fresh()), {}, tail);
        QVERIFY2(r.exitCode == 0, qPrintable(r.describe()));
        QCOMPARE(r.args(), expected);
        QString trimmed = tail;
        while (!trimmed.isEmpty() && (trimmed.front() == QLatin1Char(' ') || trimmed.front() == QLatin1Char('\t')))
            trimmed.remove(0, 1);
        QVERIFY2(r.value(QStringLiteral("raw")).endsWith(QLatin1Char(' ') + trimmed), qPrintable(r.value(QStringLiteral("raw"))));
    }

    void exitCodeAndStreamsPassThrough_data()
    {
        QTest::addColumn<int>("code");
        QTest::newRow("0") << 0;
        QTest::newRow("1") << 1;
        QTest::newRow("42") << 42;
        QTest::newRow("300") << 300;
    }

    void exitCodeAndStreamsPassThrough()
    {
        QFETCH(int, code);
        const Run r = run(m_onefile, {QStringLiteral("--exit=%1").arg(code)}, envFor(fresh()));
        QVERIFY2(r.exitCode == code, qPrintable(r.describe()));
        QVERIFY(r.childRan());
        QVERIFY2(r.err.contains(QLatin1String("child-stderr")), qPrintable(r.err));
    }

    void workingFolderAndLauncherEnvironment()
    {
        const QString cwd = m_tmp.filePath(QString::fromUtf8("work dir ünï"));
        QVERIFY(QDir().mkpath(cwd));
        const Run r = run(m_onefile, {}, envFor(fresh()), cwd);
        QVERIFY2(r.exitCode == 0, qPrintable(r.describe()));
        QCOMPARE(canonical(r.value(QStringLiteral("cwd"))), canonical(cwd));
        QCOMPARE(canonical(r.value(QStringLiteral("launcher_dir"))), canonical(m_dist));
        QCOMPARE(canonical(r.value(QStringLiteral("launcher_exe"))), canonical(m_onefile));
    }

    void laterStartsUseTheCache()
    {
        const Dirs d = fresh();
        const Run first = run(m_onefile, {}, envFor(d));
        QVERIFY2(first.exitCode == 0 && first.err.contains(QLatin1String("unpacked")), qPrintable(first.describe()));
        const QString dir = onlyRuntime(d.runtime());
        QVERIFY(!dir.isEmpty());
        QVERIFY(writeFile(dir + QStringLiteral("/sentinel.txt"), "still here"));
        const QDateTime markerTime = QFileInfo(dir + QStringLiteral("/.onefile-complete")).lastModified();

        const Run second = run(m_onefile, {QStringLiteral("--exit=7")}, envFor(d));
        QVERIFY2(second.exitCode == 7 && second.childRan(), qPrintable(second.describe()));
        QVERIFY2(second.err.contains(QLatin1String("fw-onefile: cached")), qPrintable(second.err));
        QVERIFY2(!second.err.contains(QLatin1String("unpacked")), qPrintable(second.err));
        QVERIFY(QFileInfo::exists(dir + QStringLiteral("/sentinel.txt")));
        QCOMPARE(QFileInfo(dir + QStringLiteral("/.onefile-complete")).lastModified(), markerTime);
        QCOMPARE(folders(d.runtime()).size(), 1);
    }

    void damagedCacheIsUnpackedAgain_data()
    {
        QTest::addColumn<QString>("damage");
        QTest::newRow("missing file") << QStringLiteral("delete");
        QTest::newRow("truncated file") << QStringLiteral("truncate");
        QTest::newRow("no marker") << QStringLiteral("marker");
    }

    void damagedCacheIsUnpackedAgain()
    {
        QFETCH(QString, damage);
        const Dirs d = fresh();
        QVERIFY(run(m_onefile, {}, envFor(d)).exitCode == 0);
        const QString dir = onlyRuntime(d.runtime());
        QVERIFY(!dir.isEmpty());
        const QString victim = dir + QStringLiteral("/plugins/platforms/qtext.dll");
        if (damage == QLatin1String("delete")) {
            QVERIFY(QFile::remove(victim));
        } else if (damage == QLatin1String("truncate")) {
            QFile f(victim);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.resize(1000));
        } else {
            QVERIFY(QFile::remove(dir + QStringLiteral("/.onefile-complete")));
        }
        const Run r = run(m_onefile, {}, envFor(d));
        QVERIFY2(r.exitCode == 0 && r.childRan(), qPrintable(r.describe()));
        QVERIFY2(r.err.contains(QLatin1String("unpacked")), qPrintable(r.err));
        QCOMPARE(folders(d.runtime()), QStringList{QFileInfo(dir).fileName()}); // damaged copy removed, no leftovers
        QCOMPARE(sha256Of(victim), sha256Of(m_payload + QStringLiteral("/plugins/platforms/qtext.dll")));
    }

    void damagedPayloadIsRefused_data()
    {
        QTest::addColumn<qint64>("fromEnd"); // byte to flip, counted from the end of the file
        QTest::addColumn<QString>("message");
        QTest::newRow("chunk data") << qint64(128 + 1000) << QStringLiteral("SHA-256");
        QTest::newRow("trailer") << qint64(60) << QStringLiteral("trailer checksum");
    }

    void damagedPayloadIsRefused()
    {
        QFETCH(qint64, fromEnd);
        QFETCH(QString, message);
        const QString damaged = m_tmp.filePath(QStringLiteral("damaged-%1/").arg(fromEnd)) + kEntry;
        QDir().mkpath(QFileInfo(damaged).absolutePath());
        QVERIFY(QFile::copy(m_onefile, damaged));
        {
            QFile f(damaged);
            QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.seek(f.size() - fromEnd));
            char c = 0;
            QVERIFY(f.getChar(&c));
            QVERIFY(f.seek(f.size() - fromEnd));
            QVERIFY(f.putChar(char(c ^ 0x5A)));
        }
        const Dirs d = fresh();
        const Run r = run(damaged, {}, envFor(d));
        QVERIFY2(r.exitCode == 9101, qPrintable(r.describe()));
        QVERIFY2(r.err.contains(QLatin1String("damaged")) && r.err.contains(message), qPrintable(r.err));
        QVERIFY2(!r.childRan(), qPrintable(r.out));
        for (const QString& f : folders(d.runtime()))
            QVERIFY2(!f.startsWith(QLatin1String("1.0.0-")), qPrintable(f));

        const Run v = pack({QStringLiteral("verify"), damaged});
        QVERIFY2(v.exitCode != 0, qPrintable(v.describe()));
    }

    void launcherWithoutPayloadSaysSo()
    {
        const Run r = run(QStringLiteral(FW_ONEFILE_STUB_PATH), {}, envFor(fresh()));
        QCOMPARE(r.exitCode, 9101);
        QVERIFY2(r.err.contains(QLatin1String("carries no program")), qPrintable(r.err));
    }

    void concurrentFirstStartsAllWork_data()
    {
        QTest::addColumn<bool>("lock");
        QTest::newRow("with the unpack lock") << true;
        QTest::newRow("racing for the folder name") << false;
    }

    void concurrentFirstStartsAllWork()
    {
        QFETCH(bool, lock);
        const Dirs d = fresh();
        QProcessEnvironment env = envFor(d);
        if (!lock)
            env.insert(QStringLiteral("FW_ONEFILE_NO_MUTEX"), QStringLiteral("1"));
        std::vector<std::unique_ptr<QProcess>> procs;
        for (int i = 0; i < 6; ++i) {
            procs.push_back(std::make_unique<QProcess>());
            setup(*procs.back(), m_onefile, {QStringLiteral("--exit=%1").arg(10 + i)}, env, {}, QString());
        }
        for (auto& p : procs)
            p->start();
        int unpacked = 0;
        for (int i = 0; i < int(procs.size()); ++i) {
            const Run r = finish(*procs[size_t(i)]);
            QVERIFY2(r.exitCode == 10 + i && r.childRan(), qPrintable(r.describe()));
            unpacked += r.err.contains(QLatin1String("fw-onefile: unpacked 5 files")) ? 1 : 0;
        }
        if (lock)
            QCOMPARE(unpacked, 1); // the others waited and used that copy
        else
            QVERIFY(unpacked >= 1);
        const QStringList all = folders(d.runtime());
        QVERIFY2(all.size() == 1 && all.first().startsWith(QLatin1String("1.0.0-")), qPrintable(all.join(QLatin1Char(','))));
    }

    void olderVersionsAreRemovedNewerKept()
    {
        const QString v09 = m_tmp.filePath(QStringLiteral("dist-0.9.0/")) + kEntry;
        const QString v101 = m_tmp.filePath(QStringLiteral("dist-1.0.1/")) + kEntry;
        QVERIFY(packTo(v09, QStringLiteral("0.9.0")));
        QVERIFY(packTo(v101, QStringLiteral("1.0.1")));
        const Dirs d = fresh();
        auto versions = [&] {
            QStringList v;
            for (const QString& f : folders(d.runtime()))
                v << f.left(f.lastIndexOf(QLatin1Char('-')));
            return v;
        };
        QCOMPARE(run(v09, {}, envFor(d)).exitCode, 0);
        QCOMPARE(versions(), QStringList{QStringLiteral("0.9.0")});
        const Run r = run(m_onefile, {}, envFor(d));
        QCOMPARE(r.exitCode, 0);
        QVERIFY2(r.err.contains(QLatin1String("removed old runtime 0.9.0-")), qPrintable(r.err));
        QCOMPARE(versions(), QStringList{QStringLiteral("1.0.0")});
        QCOMPARE(run(v101, {}, envFor(d)).exitCode, 0);
        QCOMPARE(versions(), QStringList{QStringLiteral("1.0.1")});
        QCOMPARE(run(m_onefile, {}, envFor(d)).exitCode, 0); // an older launcher keeps the newer copy
        QCOMPARE(versions(), (QStringList{QStringLiteral("1.0.0"), QStringLiteral("1.0.1")}));
    }

    void runtimeInUseIsKept()
    {
        const QString v09 = m_tmp.filePath(QStringLiteral("dist-0.9.0-inuse/")) + kEntry;
        QVERIFY(packTo(v09, QStringLiteral("0.9.0")));
        const Dirs d = fresh();
        // The old program stays alive until this file exists, however long the
        // new launcher takes to unpack and to look at the old runtime.
        const QString release = m_tmp.filePath(QStringLiteral("release-0.9.0-inuse"));
        QProcess running;
        setup(running, v09, {QStringLiteral("--wait-file=") + QDir::toNativeSeparators(release)}, envFor(d), {}, QString());
        const auto letGo = qScopeGuard([&] {
            writeFile(release, "go");
            if (running.state() != QProcess::NotRunning && !running.waitForFinished(60000))
                running.kill();
        });
        running.start();
        QVERIFY(running.waitForStarted(30000));
        QByteArray early; // the old program is running once it has printed everything
        QTRY_VERIFY_WITH_TIMEOUT((early += running.readAllStandardOutput()).contains("\nend\n"), 120000);

        const Run r = run(m_onefile, {}, envFor(d));
        QVERIFY2(r.exitCode == 0, qPrintable(r.describe()));
        QVERIFY2(r.err.contains(QLatin1String("kept old runtime in use 0.9.0-")), qPrintable(r.err));
        QCOMPARE(folders(d.runtime()).size(), 2);

        QVERIFY(writeFile(release, "go"));
        const Run old = finish(running);
        QVERIFY2(old.exitCode == 0, qPrintable(old.describe()));
        const Run again = run(m_onefile, {}, envFor(d));
        QVERIFY2(again.exitCode == 0, qPrintable(again.describe()));
        QVERIFY2(again.err.contains(QLatin1String("removed old runtime 0.9.0-")), qPrintable(again.err));
        QCOMPARE(folders(d.runtime()).size(), 1);
    }

    void unwritableLocalAppDataFallsBackToTemp()
    {
        Dirs d = fresh();
        QDir(d.local).removeRecursively();
        QVERIFY(writeFile(d.local, "a file where the folder should be"));
        const Run r = run(m_onefile, {}, envFor(d));
        QVERIFY2(r.exitCode == 0 && r.childRan(), qPrintable(r.describe()));
        QVERIFY2(r.err.contains(QLatin1String("cannot write to")), qPrintable(r.err));
        const QString dir = onlyRuntime(d.temp + QLatin1Char('/') + kRuntimeTail);
        QVERIFY(!dir.isEmpty());
        QCOMPARE(canonical(r.value(QStringLiteral("exe"))), canonical(dir + QLatin1Char('/') + kEntry));

        const Run cached = run(m_onefile, {}, envFor(d));
        QVERIFY2(cached.err.contains(QLatin1String("fw-onefile: cached")), qPrintable(cached.err));
    }

    void portableModeKeepsTheRuntimeNextToTheExe()
    {
        const QString app = m_tmp.filePath(QString::fromUtf8("portable app ü"));
        QVERIFY(QDir().mkpath(app));
        const QString exe = app + QLatin1Char('/') + kEntry;
        QVERIFY(QFile::copy(m_onefile, exe));
        QVERIFY(writeFile(app + QStringLiteral("/portable.txt"), QByteArray()));
        const Dirs d = fresh();
        const Run r = run(exe, {}, envFor(d));
        QVERIFY2(r.exitCode == 0 && r.childRan(), qPrintable(r.describe()));
        const QString dir = onlyRuntime(app + QStringLiteral("/exFAT Image Builder.data/runtime"));
        QVERIFY(!dir.isEmpty());
        QCOMPARE(canonical(r.value(QStringLiteral("exe"))), canonical(dir + QLatin1Char('/') + kEntry));
        QCOMPARE(canonical(r.value(QStringLiteral("launcher_dir"))), canonical(app));
        QVERIFY(!QFileInfo::exists(d.runtime()));
    }

    // cmd.exe-style start without redirection: output with arguments reaches
    // the console; a plain start leaves the console alone.
    void consoleWithoutRedirectionShowsOutput()
    {
        const Run r = runInConsole({QStringLiteral("--hello"), QStringLiteral("--exit=5")});
        QVERIFY2(r.exitCode == 0, qPrintable(r.describe()));
        QVERIFY2(r.out.contains(QLatin1String("arg=--hello")), qPrintable(r.out));
        QVERIFY2(r.out.contains(QLatin1String("child-stderr")), qPrintable(r.out));
        QVERIFY2(r.out.contains(QLatin1String("exit=5")), qPrintable(r.out));
    }

    void plainStartFromConsoleLeavesItAlone()
    {
        const Run r = runInConsole({});
        QVERIFY2(r.exitCode == 0 && r.out.contains(QLatin1String("exit=0")), qPrintable(r.describe()));
        QVERIFY2(!r.out.contains(QLatin1String("argc=")) && !r.out.contains(QLatin1String("fw-onefile")), qPrintable(r.out));
    }

    void packerListsAndVerifies()
    {
        const Run list = pack({QStringLiteral("list"), m_onefile});
        QVERIFY2(list.exitCode == 0, qPrintable(list.describe()));
        QVERIFY(list.out.contains(QLatin1String("entry exFAT Image Builder.exe")));
        QVERIFY(list.out.contains(QLatin1String("version 1.0.0")));
        bool bigStored = false, textCompressed = false;
        for (const QString& line : list.out.split(QLatin1Char('\n'))) {
            if (line.endsWith(QLatin1String("engines/backport/big.bin")))
                bigStored = line.contains(QLatin1String(" store "));
            if (line.endsWith(QLatin1String("plugins/platforms/qtext.dll")))
                textCompressed = line.contains(QLatin1String(" lzms "));
        }
        QVERIFY2(bigStored && textCompressed, qPrintable(list.out));
        const Run v = pack({QStringLiteral("verify"), m_onefile});
        QVERIFY2(v.exitCode == 0 && v.out.contains(QLatin1String("OK")), qPrintable(v.describe()));
    }

    void packerRefusesAPackedStub()
    {
        const Run r = pack({QStringLiteral("pack"), QStringLiteral("--stub"), m_onefile, QStringLiteral("--input"), m_payload,
                            QStringLiteral("--entry"), kEntry, QStringLiteral("--output"),
                            m_tmp.filePath(QStringLiteral("twice.exe"))});
        QVERIFY(r.exitCode != 0);
        QVERIFY2(r.err.contains(QLatin1String("already carries a payload")), qPrintable(r.err));
    }

    // A file with a reserved device name can be created through a \\?\ path,
    // but Explorer could neither open nor delete it in the unpacked program:
    // the packer refuses it. Wrong arguments are answered with the usage.
    void packerRefusesReservedNamesAndBadArguments()
    {
        const QString input = m_tmp.filePath(QStringLiteral("reserved-in"));
        QVERIFY(writeFile(input + QLatin1Char('/') + kEntry, "x"));
        const std::wstring reserved =
            (QStringLiteral("\\\\?\\") + QDir::toNativeSeparators(input) + QStringLiteral("\\aux.txt")).toStdWString();
        const HANDLE h = CreateFileW(reserved.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(h != INVALID_HANDLE_VALUE);
        CloseHandle(h);
        const Run r = pack({QStringLiteral("pack"), QStringLiteral("--stub"), QStringLiteral(FW_ONEFILE_STUB_PATH),
                            QStringLiteral("--input"), input, QStringLiteral("--entry"), kEntry, QStringLiteral("--output"),
                            m_tmp.filePath(QStringLiteral("reserved.exe"))});
        QVERIFY(DeleteFileW(reserved.c_str())); // QTemporaryDir could not remove it
        QVERIFY(r.exitCode != 0);
        QVERIFY2(r.err.contains(QLatin1String("unsupported file name: aux.txt")), qPrintable(r.describe()));
        QVERIFY(!QFile::exists(m_tmp.filePath(QStringLiteral("reserved.exe"))));

        const Run usage = pack({QStringLiteral("list")});
        QVERIFY(usage.exitCode != 0);
        QVERIFY2(usage.err.contains(QLatin1String("usage: fw_onefile_pack list")), qPrintable(usage.describe()));
    }
};

#endif // FW_ONEFILE_STUB_PATH

QTEST_GUILESS_MAIN(TstOnefile)
#include "tst_onefile.moc"
