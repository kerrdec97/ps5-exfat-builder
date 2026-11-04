#pragma once

// SELF reader / unwrapper ("decrypt" of an unencrypted fake-signed SELF).
//
// Derivative of make_fself.py by john-tornblom (ps5-payload-dev/sdk, GNU GPL
// version 3) via decrypt_fself.py by Nazky; reimplemented from the format
// description in docs/BACKPORT_PARITY.md §2.7 with the C++ checks listed
// there (encryption/compression bits, bounds, phdrs at e_phoff, version
// tail only when its size matches). Distributed under the GNU GPL version 3.

#include "core/Result.h"
#include "engines/backport/ElfImage.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QStringList>

#include <optional>
#include <vector>

namespace fw::backport {

enum class SelfKind { FakeSigned, Encrypted, Compressed, Malformed };

QString selfKindName(SelfKind kind);

struct SelfEntry {
    int index = 0;
    quint64 props = 0;
    quint64 offset = 0;
    quint64 filesz = 0;
    quint64 memsz = 0;
    bool isData() const;
    bool isEncrypted() const;
    bool isCompressed() const;
    int segmentIndex() const;
};

struct SelfInfo {
    SelfKind kind = SelfKind::Malformed;
    QString problem; // why it is not FakeSigned
    quint32 magic = 0;
    quint8 version = 0, mode = 0, endian = 0, attributes = 0;
    quint32 keyType = 0;
    quint16 headerSize = 0, metaSize = 0;
    quint64 fileSize = 0;
    quint16 numEntries = 0, flags = 0;
    std::vector<SelfEntry> entries;
    bool elfValid = false;
    quint64 elfOffset = 0;
    ElfHeader elf;
    std::vector<ProgramHeader> phdrs;
    std::optional<quint64> paid, ptype, appVersion, fwVersion;
    QByteArray digest;
};

namespace selfio {
// 4F 15 3D 1D or 54 14 F5 EE.
bool hasSelfMagic(QByteArrayView data);

// Parses and classifies a SELF. Fails only when `data` is not a SELF at all
// (no magic or no room for the common header); otherwise `kind` says what it is.
Result<SelfInfo> inspect(QByteArrayView data);

// Rebuilds the ELF from a fake-signed SELF. Errors: SelfEncrypted,
// SelfCompressed, SelfMalformed. Warnings (e.g. a skipped version tail) are
// appended to `warnings`.
Result<QByteArray> unwrap(QByteArrayView data, QStringList* warnings = nullptr);
} // namespace selfio

} // namespace fw::backport
