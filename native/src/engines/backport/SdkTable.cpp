#include "engines/backport/SdkTable.h"

#include "engines/backport/NidPatch.h"

#include <QCryptographicHash>
#include <QtEndian>

namespace fw::backport {

const std::array<SdkPair, kPairCount>& sdkPairs()
{
    // Pairs 1-10: the pinned SDK patcher's table. Pairs 11-13: the
    // process-param values of real retail eboots, recorded by the owner's
    // Backport engine (sdk_tool.py EXTRA_PAIRS, read 2026-10-17 from the
    // original packages; the same scan reproduces pairs 1-10 exactly):
    // 11 = 44 titles (e.g. Dying Light 2 01.090.000), 12 = 33 (e.g. Demon's
    // Souls 01.007.000), 13 = 3 (e.g. Marvel's Wolverine 01.001.005).
    static const std::array<SdkPair, kPairCount> pairs = {{
        {1, 0x01000050u, 0x07590001u},
        {2, 0x02000009u, 0x08050001u},
        {3, 0x03000027u, 0x08540001u},
        {4, 0x04000031u, 0x09040001u},
        {5, 0x05000033u, 0x09590001u},
        {6, 0x06000038u, 0x10090001u},
        {7, 0x07000038u, 0x10590001u},
        {8, 0x08000041u, 0x11090001u},
        {9, 0x09000040u, 0x11590001u},
        {10, 0x10000040u, 0x12090001u},
        {11, 0x11000040u, 0x12590001u},
        {12, 0x12000038u, 0x13090001u},
        {13, 0x13000035u, 0x13590001u},
    }};
    return pairs;
}

const SdkPair* sdkPair(int n)
{
    for (const SdkPair& p : sdkPairs())
        if (p.n == n)
            return &p;
    return nullptr;
}

static std::optional<int> bcdByte(quint32 b)
{
    const quint32 hi = (b >> 4) & 0xF, lo = b & 0xF;
    if (hi > 9 || lo > 9)
        return std::nullopt;
    return int(hi * 10 + lo);
}

std::optional<int> bcdMajor(quint32 value) { return bcdByte((value >> 24) & 0xFF); }
std::optional<int> bcdMinor(quint32 value) { return bcdByte((value >> 16) & 0xFF); }

QString sdkVersionText(quint32 value)
{
    const auto ma = bcdMajor(value), mi = bcdMinor(value);
    if (!ma || !mi)
        return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0'));
    return QStringLiteral("%1.%2").arg(*ma).arg(*mi, 2, 10, QLatin1Char('0'));
}

QString firmwareLabel(int n)
{
    return QStringLiteral("SDK %1 · loads on FW %1.00+").arg(n);
}

QString targetKey(int n)
{
    return QStringLiteral("%1xx").arg(n);
}

QByteArray tableHash()
{
    QCryptographicHash h(QCryptographicHash::Sha256);
    for (const SdkPair& p : sdkPairs()) {
        char buf[12];
        qToLittleEndian<quint32>(quint32(p.n), buf);
        qToLittleEndian<quint32>(p.ps5, buf + 4);
        qToLittleEndian<quint32>(p.ps4, buf + 8);
        h.addData(QByteArrayView(buf, sizeof buf));
    }
    for (const NidSubstitution& s : nidSubstitutions()) {
        h.addData(s.from);
        h.addData(s.to);
        const char boundary = char(s.boundarySdk);
        h.addData(QByteArrayView(&boundary, 1));
    }
    return h.result().toHex();
}

} // namespace fw::backport
