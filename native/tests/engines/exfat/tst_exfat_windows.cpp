// Interoperability with the real Windows exFAT driver (Windows CI only; needs
// administrator rights for Mount-DiskImage, which GitHub's Windows runners
// have; enable with FW_WINDOWS_MOUNT_TEST=1).
//
//  1. Windows reads our images: a native image is wrapped in a fixed VHD with
//     an MBR partition, mounted by Windows, every file compared byte for byte
//     through the drive letter, and chkdsk must report no problems.
//  2. We read Windows' images: Windows formats an exFAT volume and copies a
//     tree onto it; our reader extracts the raw volume and the result must
//     match the source byte for byte.

#include "core/FileUtil.h"
#include "core/ProcessRunner.h"
#include "core/Settings.h"
#include "engines/exfat/Module.h"
#include "engines/general/Module.h"
#include "jobs/JobQueue.h"
#include "support/TestSupport.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

using namespace fw;

namespace {

constexpr qint64 kPartitionStart = 2048; // sectors (1 MiB alignment)

QByteArray mbrFor(qint64 volumeSectors)
{
    QByteArray mbr(512, '\0');
    const quint32 sig = QRandomGenerator::global()->generate() | 1u;
    qToLittleEndian(sig, mbr.data() + 440);
    char* e = mbr.data() + 446;
    e[0] = 0x00;
    const auto byte = [](unsigned v) { return static_cast<char>(static_cast<unsigned char>(v)); };
    e[1] = byte(0xFE); e[2] = byte(0xFF); e[3] = byte(0xFF); // CHS start: use LBA
    e[4] = 0x07;                                             // exFAT/NTFS
    e[5] = byte(0xFE); e[6] = byte(0xFF); e[7] = byte(0xFF);
    qToLittleEndian(quint32(kPartitionStart), e + 8);
    qToLittleEndian(quint32(volumeSectors), e + 12);
    mbr[510] = byte(0x55);
    mbr[511] = byte(0xAA);
    return mbr;
}

// Fixed-VHD footer (Virtual Hard Disk Image Format Specification).
QByteArray vhdFooter(qint64 diskBytes)
{
    QByteArray f(512, '\0');
    char* p = f.data();
    std::memcpy(p, "conectix", 8);
    qToBigEndian(quint32(2), p + 8);
    qToBigEndian(quint32(0x00010000), p + 12);
    qToBigEndian(quint64(0xFFFFFFFFFFFFFFFFull), p + 16);
    qToBigEndian(quint32(QDateTime::currentSecsSinceEpoch() - 946684800LL), p + 24); // seconds since 2000-01-01 UTC
    std::memcpy(p + 28, "fwEB", 4);
    qToBigEndian(quint32(0x00010000), p + 32);
    std::memcpy(p + 36, "Wi2k", 4);
    qToBigEndian(quint64(diskBytes), p + 40);
    qToBigEndian(quint64(diskBytes), p + 48);
    qint64 total = diskBytes / 512;
    if (total > 65535LL * 16 * 255)
        total = 65535LL * 16 * 255;
    qint64 spt, heads, cth;
    if (total >= 65535LL * 16 * 63) {
        spt = 255;
        heads = 16;
        cth = total / spt;
    } else {
        spt = 17;
        cth = total / spt;
        heads = std::max<qint64>(4, (cth + 1023) / 1024);
        if (cth >= heads * 1024 || heads > 16) {
            spt = 31;
            heads = 16;
            cth = total / spt;
        }
        if (cth >= heads * 1024) {
            spt = 63;
            heads = 16;
            cth = total / spt;
        }
    }
    qToBigEndian(quint16(cth / heads), p + 56);
    p[58] = char(heads);
    p[59] = char(spt);
    qToBigEndian(quint32(2), p + 60); // fixed disk
    for (int i = 0; i < 16; ++i)
        p[68 + i] = char(QRandomGenerator::global()->generate() & 0xFF);
    quint32 sum = 0;
    for (int i = 0; i < 512; ++i)
        sum += quint8(p[i]);
    qToBigEndian(quint32(~sum), p + 64);
    return f;
}

// Wraps a bare volume into "<vhd>": MBR + 1 MiB gap + volume + padding + footer.
bool wrapInVhd(const QString& volume, const QString& vhd, qint64 extraBytes = 0)
{
    QFile in(volume), out(vhd);
    if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const qint64 volSectors = (in.size() + extraBytes + 511) / 512;
    out.write(mbrFor(volSectors));
    out.write(QByteArray(int((kPartitionStart - 1) * 512), '\0'));
    QByteArray buf;
    while (!(buf = in.read(4 << 20)).isEmpty())
        out.write(buf);
    const qint64 diskBytes = (kPartitionStart + volSectors) * 512;
    out.resize(diskBytes);
    out.seek(diskBytes);
    out.write(vhdFooter(diskBytes));
    return out.error() == QFile::NoError;
}

QString powershell()
{
    return QStringLiteral("C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe");
}

Result<QString> ps(const QString& script, int timeoutMs = 180000)
{
    ProcessSpec spec;
    spec.program = powershell();
    spec.arguments = {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-ExecutionPolicy"),
                      QStringLiteral("Bypass"), QStringLiteral("-Command"), script};
    spec.timeout = std::chrono::milliseconds(timeoutMs);
    auto r = runProcess(spec, CancellationToken());
    if (!r.ok())
        return r.takeError();
    if (r->exitCode != 0)
        return Error(ErrorCode::ExternalToolFailed, QStringLiteral("PowerShell failed"), r->output);
    return r->output.trimmed();
}

// Mounts a VHD and returns the partition's drive letter ("X").
Result<QString> mountVhd(const QString& vhd)
{
    auto out = ps(QStringLiteral(
                      "$ErrorActionPreference='Stop';"
                      "$img = Mount-DiskImage -ImagePath '%1' -PassThru;"
                      "$disk = $img | Get-Disk;"
                      "if ($disk.IsOffline) { Set-Disk -Number $disk.Number -IsOffline $false };"
                      "if ($disk.IsReadOnly) { Set-Disk -Number $disk.Number -IsReadOnly $false };"
                      "Start-Sleep -Seconds 2;"
                      "$part = Get-Partition -DiskNumber $disk.Number | Where-Object { $_.Type -ne 'Reserved' } | Select-Object -First 1;"
                      "if (-not $part.DriveLetter) { $part | Add-PartitionAccessPath -AssignDriveLetter; Start-Sleep -Seconds 2;"
                      "  $part = Get-Partition -DiskNumber $disk.Number -PartitionNumber $part.PartitionNumber };"
                      "Write-Output ('LETTER=' + $part.DriveLetter)")
                      .arg(QDir::toNativeSeparators(vhd)));
    if (!out.ok())
        return out;
    const int at = out->lastIndexOf(QStringLiteral("LETTER="));
    if (at < 0)
        return Error(ErrorCode::ExternalToolFailed, QStringLiteral("no drive letter"), out.value());
    return out->mid(at + 7, 1);
}

void dismountVhd(const QString& vhd)
{
    (void)ps(QStringLiteral("Dismount-DiskImage -ImagePath '%1' | Out-Null").arg(QDir::toNativeSeparators(vhd)));
}

QMap<QString, QByteArray> snapshot(const QString& root, const QStringList& skipTop = {})
{
    QMap<QString, QByteArray> out;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString rel = QDir(root).relativeFilePath(it.filePath());
        if (skipTop.contains(rel.section(u'/', 0, 0), Qt::CaseInsensitive))
            continue;
        QFile f(it.filePath());
        if (f.open(QIODevice::ReadOnly))
            out.insert(rel, f.readAll());
    }
    return out;
}

struct Env {
    std::shared_ptr<SettingsSchema> schema = std::make_shared<SettingsSchema>();
    DependencyRegistry deps;
    JobFactory jobs;
    std::unique_ptr<SettingsStore> store;
    std::unique_ptr<JobQueue> queue;
    explicit Env(const QString& dir)
    {
        ModuleContext ctx{*schema, deps, jobs};
        general::registerModule(ctx);
        exfat::registerModule(ctx);
        store = std::make_unique<SettingsStore>(schema, dir + QStringLiteral("/settings.json"));
        (void)store->load();
        queue = std::make_unique<JobQueue>(*store, jobs);
    }
    std::optional<JobRecord> run(const QString& kind, const QJsonObject& params)
    {
        auto id = queue->enqueue(kind, params);
        if (!id.ok()) {
            qWarning() << id.error().toLogString();
            return std::nullopt;
        }
        (void)QTest::qWaitFor([&] { auto r = queue->record(id.value()); return r && isTerminal(r->state); }, 300000);
        return queue->record(id.value());
    }
};

} // namespace

class TstExfatWindows : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    QString m_src;

private slots:
    void initTestCase()
    {
#ifndef Q_OS_WIN
        QSKIP("Windows only");
#endif
        if (qEnvironmentVariable("FW_WINDOWS_MOUNT_TEST") != QLatin1String("1"))
            QSKIP("Set FW_WINDOWS_MOUNT_TEST=1 (needs administrator rights for Mount-DiskImage)");
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        m_src = m_root.filePath(QStringLiteral("PPSA09999"));
        QVERIFY(fwtest::writeTextFile(m_src + QStringLiteral("/eboot.bin"), QByteArray(100000, 'E')));
        QVERIFY(fwtest::writeTextFile(m_src + QStringLiteral("/sce_sys/param.json"), R"({"titleId":"PPSA09999"})"));
        QVERIFY(fwtest::writeRandomFile(m_src + QStringLiteral("/data/big.bin"), 9 * 1024 * 1024 + 333, 7));
        QVERIFY(fwtest::writeTextFile(m_src + QStringLiteral("/data/Ünïcödé — 日本語.txt"), "unicode"));
        QVERIFY(fwtest::writeTextFile(m_src + QStringLiteral("/data/empty.bin"), QByteArray()));
        for (int i = 0; i < 300; ++i)
            QVERIFY(fwtest::writeTextFile(m_src + QStringLiteral("/many/f%1.txt").arg(i), QByteArray::number(i).repeated(i + 1)));
        QVERIFY(fwtest::writeTextFile(m_src + QStringLiteral("/") + QString(200, u'n') + QStringLiteral(".bin"), "long name"));
        QDir().mkpath(m_src + QStringLiteral("/empty folder"));
    }

    void windowsReadsNativeImages()
    {
        Env env(m_root.filePath(QStringLiteral("e1")));
        const QString image = m_root.filePath(QStringLiteral("native.exfat"));
        auto built = env.run(exfat::kJobBuild, {{QStringLiteral("source"), m_src}, {QStringLiteral("output"), image}});
        QVERIFY2(built && built->state == JobState::Succeeded, built && built->error ? qPrintable(built->error->toLogString()) : "");
        const QString vhd = m_root.filePath(QStringLiteral("native.vhd"));
        QVERIFY(wrapInVhd(image, vhd));
        auto letter = mountVhd(vhd);
        QVERIFY2(letter.ok() && letter->size() == 1, letter.ok() ? qPrintable(letter.value()) : qPrintable(letter.error().toLogString()));
        const QString drive = letter.value() + QStringLiteral(":/");
        const auto expected = snapshot(m_src);
        const auto got = snapshot(drive, {QStringLiteral("System Volume Information")});
        auto chk = ps(QStringLiteral("chkdsk %1: ; exit $LASTEXITCODE").arg(letter.value()));
        dismountVhd(vhd);
        QCOMPARE(got.keys(), expected.keys());
        QVERIFY2(got == expected, "file contents differ through the Windows exFAT driver");
        QVERIFY2(chk.ok(), chk.ok() ? "" : qPrintable(chk.error().detail));
        qInfo().noquote() << "chkdsk:" << chk.value().right(300);
    }

    void nativeReaderReadsWindowsImages()
    {
        // A blank fixed VHD with one 128 MiB partition, formatted by Windows.
        const qint64 volBytes = 128LL * 1024 * 1024;
        const QString blankVol = m_root.filePath(QStringLiteral("blank.vol"));
        {
            QFile f(blankVol);
            QVERIFY(f.open(QIODevice::WriteOnly));
            QVERIFY(f.resize(volBytes));
        }
        const QString vhd = m_root.filePath(QStringLiteral("windows.vhd"));
        QVERIFY(wrapInVhd(blankVol, vhd));
        auto letter = mountVhd(vhd);
        QVERIFY2(letter.ok() && letter->size() == 1, letter.ok() ? qPrintable(letter.value()) : qPrintable(letter.error().toLogString()));
        auto fmt = ps(QStringLiteral("Format-Volume -DriveLetter %1 -FileSystem exFAT -AllocationUnitSize 32768 -NewFileSystemLabel WINFMT -Force -Confirm:$false | Out-Null;"
                                     "Copy-Item -Recurse -Force -Path '%2\\*' -Destination '%1:\\'")
                          .arg(letter.value(), QDir::toNativeSeparators(m_src)));
        dismountVhd(vhd);
        QVERIFY2(fmt.ok(), fmt.ok() ? "" : qPrintable(fmt.error().detail));

        // Cut the partition out of the VHD and read it with the native reader.
        const QString volume = m_root.filePath(QStringLiteral("windows.exfat"));
        {
            QFile in(vhd), out(volume);
            QVERIFY(in.open(QIODevice::ReadOnly) && out.open(QIODevice::WriteOnly));
            QVERIFY(in.seek(kPartitionStart * 512));
            qint64 left = volBytes;
            while (left > 0) {
                const QByteArray b = in.read(std::min<qint64>(left, 4 << 20));
                QVERIFY(!b.isEmpty());
                out.write(b);
                left -= b.size();
            }
        }
        Env env(m_root.filePath(QStringLiteral("e2")));
        const QString out = m_root.filePath(QStringLiteral("extracted"));
        auto ex = env.run(exfat::kJobExtract, {{QStringLiteral("image"), volume}, {QStringLiteral("output"), out}});
        QVERIFY2(ex && ex->state == JobState::Succeeded, ex && ex->error ? qPrintable(ex->error->toLogString()) : "");
        const auto expected = snapshot(m_src);
        const auto got = snapshot(out, {QStringLiteral("System Volume Information")});
        QCOMPARE(got.keys(), expected.keys());
        QVERIFY2(got == expected, "the native reader disagrees with the Windows exFAT driver");
    }
};

QTEST_GUILESS_MAIN(TstExfatWindows)
#include "tst_exfat_windows.moc"
