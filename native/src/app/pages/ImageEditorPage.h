#pragma once

// Edit image (legacy Edit page): open an .exfat or .ffpkg image, browse its
// tree (read on a worker thread), stage add / replace / delete / new folder
// / rename operations, review them and apply through image.<fmt>.edit; and
// "Convert to…" through image.convert.

#include "app/pages/Page.h"
#include "app/services/ImageTree.h"

#include <QJsonArray>
#include <QTreeWidget>

namespace fw::pages {

struct StagedOp {
    QString op;        // add | replace | delete | mkdir | rename
    QString imagePath; // '/'-separated, no leading slash
    QString localPath; // add / replace
    QString newName;   // rename (ffpkg only)
    QString describe() const;
};

class ImageEditorPage : public JobFormPage {
    Q_OBJECT
public:
    ImageEditorPage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent = nullptr);
    bool acceptsDrop(const QList<QUrl>& urls) const override;
    void dropUrls(const QList<QUrl>& urls) override;
    void navigateWith(const QVariantMap& args) override;
    void refresh() override;
    QString statusContext() const override;
    QStringList demoStates() const override { return {}; }
    void open(const QString& image);
    // The operations in the engine's JSON shape (tests).
    QJsonArray operationsJson() const;
    const std::vector<StagedOp>& staged() const { return m_ops; }
    void stage(const StagedOp& op);
    void clearStaged();
    QString format() const { return m_tree.format; }
    QTreeWidget* tree() { return m_treeView; }

protected:
    Result<QString> submit(bool buildNow) override;
    QString validateForm() override;

private:
    void onTree(const ui::ImageTreeResult& r);
    void rebuildTree();
    void rebuildStaged();
    QString selectedPath(bool* isDir = nullptr) const;
    void addFiles(bool folder);
    void replaceSelected();
    void deleteSelected();
    void newFolder();
    void renameSelected();
    void convert();

    ui::ImageTreeReader* m_reader;
    ui::ImageTreeResult m_tree;
    std::vector<StagedOp> m_ops;
    ui::PathField* m_image;
    ui::Badge* m_badge;
    QLabel* m_summary;
    QTreeWidget* m_treeView;
    ui::EmptyState* m_treeEmpty;
    QPushButton* m_btnAddFiles;
    QPushButton* m_btnAddFolder;
    QPushButton* m_btnReplace;
    QPushButton* m_btnDelete;
    QPushButton* m_btnMkdir;
    QPushButton* m_btnRename;
    ui::Card* m_stagedCard;
    QVBoxLayout* m_stagedLayout;
    QLabel* m_stagedCaption;
    QPushButton* m_clear;
    ui::Segmented* m_convertFormat;
    ui::PathField* m_convertOutput;
    QPushButton* m_convert;
};

} // namespace fw::pages
