#pragma once

// Cross-engine workflows that no single engine owns:
//
//  * FPKG builds straight from .exfat / .ffpkg / .ffpfs(c) images: registers
//    the image readers as fw::fpkg source stagers.
//  * image.convert — convert an image to another format (legacy Convert tab):
//    extract with the source format's reader, build with the target format's
//    writer, verify the new image against the extracted tree.
//      params {image, format: "exfat"|"ffpkg"|"ffpfsc", output (file path)}
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
