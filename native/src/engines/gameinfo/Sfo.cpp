#include "engines/gameinfo/Sfo.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>

namespace fw::gameinfo {

namespace {

constexpr qint64 kMaxSfoBytes = 16 * 1024 * 1024;

quint32 u32At(const QByteArray& d, qsizetype off)
{
    return qFromLittleEndian<quint32>(d.constData() + off);
}
quint16 u16At(const QByteArray& d, qsizetype off)
{
    return qFromLittleEndian<quint16>(d.constData() + off);
}

} // namespace

QString SfoEntry::text() const
{
    QByteArray b = raw;
    while (!b.isEmpty() && b.back() == '\0')
        b.chop(1);
    return QString::fromUtf8(b);
}

quint32 SfoEntry::integer() const
{
    if (!isInteger() || raw.size() < 4)
        return 0;
    return u32At(raw, 0);
}

const SfoEntry* SfoFile::find(const QString& key) const
{
    for (const auto& e : entries)
        if (e.key == key)
            return &e;
    return nullptr;
}

QString SfoFile::string(const QString& key) const
{
    const SfoEntry* e = find(key);
    return e && e->isString() ? e->text() : QString();
}

std::optional<quint32> SfoFile::integer(const QString& key) const
{
    const SfoEntry* e = find(key);
    if (!e || !e->isInteger() || e->raw.size() < 4)
        return std::nullopt;
    return e->integer();
}

Result<SfoFile> parseSfo(const QByteArray& data)
{
    if (data.size() < 20)
        return Error(ErrorCode::CorruptData, QStringLiteral("param.sfo is too small to be valid."),
                     QStringLiteral("%1 bytes").arg(data.size()));
    const quint32 magic = u32At(data, 0);
    if (magic != kSfoMagic)
        return Error(ErrorCode::CorruptData, QStringLiteral("param.sfo has an unknown format."),
                     QStringLiteral("magic 0x%1").arg(magic, 8, 16, QLatin1Char('0')));
    SfoFile out;
    out.version = u32At(data, 4);
    const quint32 keyTable = u32At(data, 8);
    const quint32 dataTable = u32At(data, 12);
    const quint32 count = u32At(data, 16);
    if (keyTable > quint32(data.size()) || dataTable > quint32(data.size()))
        return Error(ErrorCode::CorruptData, QStringLiteral("param.sfo is damaged (its tables point outside the file)."));

    const qsizetype size = data.size();
    for (quint32 i = 0; i < count; ++i) {
        const qsizetype base = 20 + qsizetype(i) * 16;
        if (base + 16 > size)
            break; // legacy: stop at the end of the entry table
        SfoEntry e;
        const quint16 keyOff = u16At(data, base);
        e.format = u16At(data, base + 2);
        e.length = u32At(data, base + 4);
        e.maxLength = u32At(data, base + 8);
        const quint32 dataOff = u32At(data, base + 12);

        const qsizetype keyStart = qsizetype(keyTable) + keyOff;
        const qsizetype keyEnd = keyStart < size ? data.indexOf('\0', keyStart) : -1;
        const qsizetype valStart = qsizetype(dataTable) + qsizetype(dataOff);
        const qsizetype valLen = e.isInteger() ? 4 : qsizetype(e.length);
        if (keyEnd < 0 || valStart > size || valStart + valLen > size) {
            ++out.skippedEntries;
            continue;
        }
        e.key = QString::fromUtf8(data.constData() + keyStart, keyEnd - keyStart);
        e.raw = data.mid(valStart, valLen);
        out.entries.push_back(std::move(e));
    }
    return out;
}

Result<SfoFile> readSfo(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::NotFound, QStringLiteral("param.sfo could not be opened."), f.errorString());
    if (f.size() > kMaxSfoBytes)
        return Error(ErrorCode::CorruptData, QStringLiteral("param.sfo is too large to be valid."));
    return parseSfo(f.readAll());
}

} // namespace fw::gameinfo
