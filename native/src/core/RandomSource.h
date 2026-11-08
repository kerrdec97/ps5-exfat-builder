#pragma once

// Random-access bytes that are not a plain file on disk, for example an image
// stored inside another image (an .exfat or .ffpkg inside an .ffpfsc). Image
// readers take a RandomSource through SourceDevice, a read-only QIODevice, so
// they read it exactly as they read an image file.
//
// readAt() must be safe to call from several threads (each reader thread has
// its own SourceDevice and position).

#include "core/Result.h"

#include <QIODevice>
#include <QString>

#include <memory>

namespace fw {

class RandomSource {
public:
    virtual ~RandomSource() = default;
    virtual qint64 size() const = 0;
    // Reads exactly `len` bytes at `offset` (fails past the end).
    virtual Status readAt(qint64 offset, char* dst, qint64 len) = 0;
    // For messages: "PPSA01234.ffpkg inside Game.ffpfsc".
    virtual QString describe() const = 0;
};

class SourceDevice final : public QIODevice {
public:
    explicit SourceDevice(std::shared_ptr<RandomSource> source, QObject* parent = nullptr);
    bool isSequential() const override { return false; }
    qint64 size() const override;
    const std::shared_ptr<RandomSource>& source() const { return m_source; }

protected:
    qint64 readData(char* data, qint64 maxSize) override;
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    std::shared_ptr<RandomSource> m_source;
};

// Opens a SourceDevice (read-only, unbuffered).
std::unique_ptr<SourceDevice> openSourceDevice(std::shared_ptr<RandomSource> source);

} // namespace fw
