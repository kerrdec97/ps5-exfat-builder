#pragma once

// One scanner for preflight, processing and reports (docs/BACKPORT_PARITY.md
// §2.3): finds every ELF/SELF executable of a game folder, classifies it
// (ELF, fake-signed SELF, encrypted / compressed / malformed SELF), reads the
// PS4/PS5 SDK values of each param, and lists what the run must know about:
// the dump's own fakelib/ and decrypted/ trees (never processed, S33), OS
// metadata (S15), leftover .bak files (S16), nested eboot.bin folders and the
// param.json kept in the originals ZIP. Read-only.

#include "core/Cancellation.h"
#include "core/Result.h"
#include "engines/backport/Common.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace fw::backport {

enum class FileClass {
    Elf,            // unsigned ELF
    Self,           // fake-signed (unencrypted) SELF
    SelfEncrypted,  // S32
    SelfCompressed, // S32
    SelfMalformed,
    ElfUnsupported, // ELF the signer cannot rewrite
    Unreadable,     // name says executable, header could not be read
};

QString fileClassName(FileClass c);

enum class Area { Game, Fakelib, Decrypted };

struct ExecFile {
    QString rel;        // '/' separators, relative to the game root
    qint64 size = 0;
    FileClass cls = FileClass::Elf;
    Area area = Area::Game;
    QString sha256;     // Full scans only
    QString problem;    // why it cannot be processed (class or param problem)
    Code problemCode = Code::Ok;
    std::optional<quint32> ps4, ps5; // first process param, else first module param
    std::optional<int> sdk;          // BCD major of ps5
    bool moduleWithoutParam = false;

    bool isProcessable() const { return area == Area::Game && (cls == FileClass::Elf || cls == FileClass::Self); }
    bool isEncryptedOrCompressed() const
    {
        return cls == FileClass::SelfEncrypted || cls == FileClass::SelfCompressed;
    }
};

struct ScanResult {
    QString root;
    bool hasEboot = false;     // eboot.bin at the root (S5)
    QString ebootRel;
    std::vector<ExecFile> executables; // every ELF/SELF, sorted by rel
    QStringList leftoverBaks;          // *.bak, kept and reported (S16)
    QStringList metadataSkipped;       // OS metadata files and folders (S15)
    QStringList skippedSymlinks;
    QStringList nestedEbootDirs;       // folders (not the root) with an eboot.bin
    QStringList fakelibFiles;          // every file under <root>/fakelib/
    QStringList decryptedFiles;        // non-empty files under decrypted/ dirs (<= 4 levels)
    qint64 decryptedBytes = 0;
    QString paramJsonRel;              // first param.json inside the game
    qint64 paramJsonBytes = 0;
    QHash<QString, QString> fileIndex; // relKey → rel for every file
    qint64 totalFiles = 0;

    const ExecFile* find(const QString& rel) const;
    const ExecFile* eboot() const;
    std::vector<const ExecFile*> processable() const;
    qint64 processableBytes() const;
    std::vector<const ExecFile*> encryptedOrCompressed() const;
    // The game's own SDK (D7): procparam of eboot.bin.
    std::optional<int> gameSdk() const;
    // Actual relative path for a case-insensitive key (empty if absent).
    QString resolve(const QString& rel) const { return fileIndex.value(relKey(rel)); }
};

enum class ScanMode {
    Full,  // read every game executable: hashes, params, SELF structure
    Quick, // header classification only; params of eboot.bin (library scans)
};

// `excludeDirs`: absolute folders never entered (S38: the ZIP folders when
// they sit under a scanned folder).
Result<ScanResult> scanGame(const QString& root, const CancellationToken& token, ScanMode mode = ScanMode::Full,
                            const QStringList& excludeDirs = {});

// Game folders (eboot.bin at their root) under `folders`, at most `depth`
// levels below each, never entering `excludeDirs` (S38) or OS metadata.
QStringList discoverGames(const QStringList& folders, int depth, const QStringList& excludeDirs,
                          const CancellationToken& token, QStringList* warnings = nullptr);

// Magic check used for fakelib libraries (ELF or either SELF magic).
bool hasExecutableMagic(const QByteArray& head);

} // namespace fw::backport
