#pragma once

// Cross-engine workflows that no single engine owns:
//
//  * FPKG builds straight from .exfat / .ffpkg / .ffpfs(c) images: registers
//    the image readers as fw::fpkg source stagers.
//  * image.convert — convert an image to another format (legacy Convert tab).
//    "repack": extract with the source format's reader, build with the target
//    format's writer, verify the new image against the extracted tree.
//    "direct" (default for .exfat/.ffpkg -> .ffpfsc): pack the image file
//    itself into a compressed PFS container under a plain ASCII inner name,
//    without a temporary copy. Such a container converts back by unpacking.
//      params {image, format: "exfat"|"ffpkg"|"ffpfs"|"ffpfsc", output (file path),
//              method?: "direct"|"repack", replaceExisting?: bool}
//  * image.compare — compare two images (.exfat / .ffpkg / .ffpfs / .ffpfsc),
//    or an image and a folder, through the native readers (ImageCompare.h):
//    files and folders only in A / only in B, different sizes and, at full
//    depth, different contents. The job succeeds whatever it finds; the
//    differences are in JobOutcome::details (shape in app/services/JobSubmit.h)
//    and the verification field is not used.
//      params {a, b, depth: "full" (default) | "structure"}
//  * ImageFiles.h — format-neutral read-only file access (also used by the
//    Image tools page's hex preview).
//  * Post-build upload to the PS5 (legacy adv_auto_upload): PostBuildActions
//    queues ps5.ftp.upload for every verified image when
//    ps5.autoUploadAfterBuild is on.

#include "jobs/JobQueue.h"
#include "jobs/Module.h"

#include <QObject>

namespace fw::integration {

void registerModule(ModuleContext& ctx);

namespace keys {
inline const QString kAutoUpload = QStringLiteral("ps5.autoUploadAfterBuild");
}

inline const QString kConvertJob = QStringLiteral("image.convert");
inline const QString kCompareJob = QStringLiteral("image.compare");

// Lives on the UI thread next to the queue.
class PostBuildActions : public QObject {
    Q_OBJECT
public:
    PostBuildActions(JobQueue& queue, SettingsStore& settings, QObject* parent = nullptr);

signals:
    void uploadQueued(const QString& buildJobId, const QString& uploadJobId);

private:
    void onFinished(const QString& id);
    JobQueue& m_queue;
    SettingsStore& m_settings;
};

} // namespace fw::integration
