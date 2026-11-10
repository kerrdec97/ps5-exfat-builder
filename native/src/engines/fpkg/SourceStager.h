#pragma once

// Image sources (.exfat, .ffpkg, .ffpfs, .ffpfsc) for FPKG builds.
//
// The FPKG engine does not read images itself. Image readers live in other
// modules; the app registers one SourceStager per image type at start-up
// (after every module registered), and an FPKG build whose source is such an
// image asks the stager to copy the image's tree into a staging folder (in a
// work folder next to the output, or under the job's temporary folder when
// the user set one). The app root inside that folder is then found by the FPKG
// engine (the first folder with sce_sys, breadth-first, at most three levels
// deep), so a stager may simply extract the whole image.
//
// A stager must:
//  * write only below `destination` (which exists and is empty);
//  * advance `progress` (bytes or files) and call `token.checkpoint()` at
//    least every 4 MiB / 100 files, returning Error::cancelled() when it is
//    false;
//  * never modify the image.

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"

#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace fw::fpkg {

class SourceStager {
public:
    virtual ~SourceStager() = default;

    // "exFAT image"
    virtual QString displayName() const = 0;
    // Lower-case extensions without the dot, e.g. {"exfat"}.
    virtual QStringList extensions() const = 0;
    // Bytes the staged tree will need (free-space check); <= 0 = unknown
    // (the image size is used).
    virtual qint64 estimateStagedBytes(const QString& image) const
    {
        Q_UNUSED(image);
        return 0;
    }
    // Cheap check on the UI thread before a job is queued (header, readable).
    virtual Status probe(const QString& image) const
    {
        Q_UNUSED(image);
        return success();
    }
    // Copies the image's tree into `destination`. `progress` is in the
    // "Prepare source" phase; call setTotal() when the size becomes known.
    virtual Status stage(const QString& image, const QString& destination, const CancellationToken& token,
                         ProgressReporter* progress) = 0;
    // True when a game FOLDER must be staged too instead of packaged as it is
    // (a zstd- or LZ4-packed game: the package gets its files unpacked).
    virtual bool handlesFolder(const QString& folder) const
    {
        Q_UNUSED(folder);
        return false;
    }
    // One line for the log: why and how the source is staged.
    virtual QString stagingNote(const QString& source) const
    {
        Q_UNUSED(source);
        return {};
    }
};

// Registration (thread-safe). A later registration for the same extension
// replaces the earlier one.
void registerSourceStager(std::shared_ptr<SourceStager> stager);
void unregisterSourceStagers(); // tests
std::shared_ptr<SourceStager> sourceStagerFor(const QString& path); // by extension, or null
// A stager that wants this game folder (handlesFolder), or null.
std::shared_ptr<SourceStager> folderStagerFor(const QString& folder);
QStringList imageSourceExtensions(); // every extension FPKG knows as an image source

// Finds the app root (a folder containing sce_sys) under `dir`, breadth-first,
// at most `maxDepth` levels below it. "" if none.
QString findAppRoot(const QString& dir, int maxDepth = 3);

} // namespace fw::fpkg
