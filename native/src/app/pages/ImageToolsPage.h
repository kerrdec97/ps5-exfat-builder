#pragma once

// Image tools (v5): one page for everything done with an existing image. It
// starts as an "Open an image" drop zone (.exfat .ffpkg .ffpfs .ffpfsc, and an
// FPKG .pkg for extract/verify); once an image is open it shows as a strip
// (format, size, title read from inside the image) and offers four actions,
// each with only its own options:
//
//   Extract  — copy the files out (image.<fmt>.extract / fpkg.extract)
//   Verify   — check the image against its own checksums (image.<fmt>.verify /
//              fpkg.verify), or compare it with a folder or another image
//              (image.compare; result in CompareResultSheet)
//   Convert  — exFAT / FFPKG / FFPFS / FFPFSC (image.convert; "direct" packs an
//              .exfat/.ffpkg into an .ffpfsc as it is)
//   Edit     — browse the tree, stage add / replace / delete / new folder /
//              rename and apply them in one job (image.<fmt>.edit); read-only
//              hex preview of a file (HexPreviewSheet)
//
// Replaces the v5.0 "Extract & verify" and "Edit image" pages; MainWindow
// sends their keys ("extract", "edit") here with the matching action.

#include "app/pages/Page.h"
#include "app/services/ImageInfo.h"
#include "app/services/ImageTree.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QTreeWidget>

#include <functional>

namespace fw::ui {
class ChoiceTiles;
class Disclosure;
class DropZone;
class Switch;
} // namespace fw::ui

namespace fw::pages {

class CompareResultSheet;
class HexPreviewSheet;

struct StagedOp {
    QString op;        // add | replace | delete | mkdir | rename
    QString imagePath; // '/'-separated, no leading slash
    QString localPath; // add / replace
    QString newName;   // rename (ffpkg only)
    QString describe() const;
};

class ImageToolsPage : public JobFormPage {
    Q_OBJECT
public:
    enum class Action { Extract, Verify, Convert, Edit };

    ImageToolsPage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent = nullptr);

    // ---- the image
    void openImage(const QString& path); // "" closes it
    QString imagePath() const { return m_path; }
    const ui::ImageSummary& imageSummary() const { return m_summary; }
    void setAction(Action action);
    Action action() const { return m_action; }
    bool isActionEnabled(Action action) const;

    // ---- Extract / Verify
    void setExtractDestination(const QString& folder);
    bool verifyOnly() const; // Verify against the image's own checksums
    bool compareMode() const; // Verify against a folder or another image

    // ---- Compare. A is the open image; `b` an image or a folder; depth "full" | "structure".
    void setCompareInputs(const QString& a, const QString& b, const QString& depth = QStringLiteral("full"));
    void startCompare();
    QString compareJobId() const { return m_compareJobId; }
    QString compareStatus() const; // the result badge's text
    QPushButton* compareButton() { return primaryButton(); }
    QPushButton* compareResultButton() { return m_compareView; }
    // Opens the result of the last comparison (non-modal); nullptr when there is none.
    CompareResultSheet* showCompareResult();

    // ---- Convert
    void setConvertTarget(const QString& format); // jobs::kFormat*
    QString convertTarget() const;
    QString convertOutput() const; // the chosen file, else the default next to the image
    void setConvertOutput(const QString& file);
    QString convertMethod() const; // "direct" | "repack" | "" (not applicable)
    void setConvertMethod(const QString& method);

    // ---- Edit
    QJsonArray operationsJson() const; // the staged operations in the engine's JSON shape
    const std::vector<StagedOp>& staged() const { return m_ops; }
    void stage(const StagedOp& op);
    void clearStaged();
    QString format() const { return m_tree.format; } // "exfat" | "ffpkg" once the tree is read
    QTreeWidget* tree() { return m_treeView; }
    // Opens the hex preview of the file at `path` in the open image (non-modal);
    // nullptr when no tree is open or `path` is not a file in it.
    HexPreviewSheet* preview(const QString& path);

    // ---- Page
    bool acceptsDrop(const QList<QUrl>& urls) const override;
    void dropUrls(const QList<QUrl>& urls) override;
    void setDragHighlight(bool on) override;
    void navigateWith(const QVariantMap& args) override;
    void refresh() override;
    QString statusContext() const override;
    QStringList demoStates() const override;
    void applyDemoState(const QString& name) override;
    void clearDemoState() override;

protected:
    Result<QString> submit(bool buildNow) override;
    QString validateForm() override;

private:
    void buildImageCard();
    void buildExtractPanel();
    void buildVerifyPanel();
    void buildConvertPanel();
    void buildEditPanel();
    void onSummary(const ui::ImageSummary& s);
    void showStrip();
    void updateActions();   // tiles enabled, panels shown, primary text, kinds
    void updateKinds();
    void updatePrimary();
    void updateAdvanced();
    void updateConvert();
    void rebuildConvertChoices();
    void ensureTree();
    // Compare
    void updateCompareKind();
    bool checkCompareInputs(QString* problem);
    void onCompareJob(const QString& id, bool finished);
    void showCompareState();
    // Edit
    void onTree(const ui::ImageTreeResult& r);
    void rebuildTree();
    void rebuildStaged();
    void updateTreeButtons();
    QString selectedPath(bool* isDir = nullptr) const;
    void addFiles(bool folder);
    void replaceSelected();
    void deleteSelected();
    void newFolder();
    void renameSelected();
    void showTreeMenu(const QPoint& pos);
    void setDemoImage(const QString& path, qint64 bytes);

    // image
    QString m_path;
    ui::ImageSummary m_summary;
    ui::ImageSummaryReader* m_info;
    Action m_action = Action::Extract;
    ui::Card* m_imageCard = nullptr;
    ui::Badge* m_imageBadge = nullptr;
    ui::PathField* m_imageField = nullptr; // the model behind the drop zone and Change…
    ui::DropZone* m_drop = nullptr;
    ui::Card* m_strip = nullptr;
    ui::IconTile* m_stripTile = nullptr;
    ui::ElideLabel* m_stripTitle = nullptr;
    ui::ElideLabel* m_stripMeta = nullptr;
    ui::ElideLabel* m_stripPath = nullptr;
    ui::MessageRow* m_imageMsg = nullptr;
    QWidget* m_actionsBar = nullptr; // ActionBar (anonymous namespace)
    QWidget* m_extractPanel = nullptr;
    QWidget* m_unpackRow = nullptr;
    ui::Switch* m_unpackPacks = nullptr;
    QWidget* m_verifyPanel = nullptr;
    QWidget* m_convertPanel = nullptr;
    QWidget* m_editPanel = nullptr;
    ui::Disclosure* m_advanced = nullptr;
    bool m_demo = false;
    QPointer<QWidget> m_demoSheet;

    // extract
    ui::PathField* m_destination = nullptr;

    // verify / compare
    ui::Segmented* m_verifyMode = nullptr;
    QLabel* m_verifyHelp = nullptr;
    QWidget* m_compareBox = nullptr;
    ui::Segmented* m_compareBKind = nullptr;
    ui::PathField* m_compareB = nullptr;
    ui::Segmented* m_compareDepth = nullptr;
    ui::Card* m_compareResultRow = nullptr;
    ui::Badge* m_compareBadge = nullptr;
    QLabel* m_compareSummary = nullptr;
    QPushButton* m_compareView = nullptr;
    QString m_compareJobId;
    struct CompareResult { // the last finished comparison, kept when its queue entry is cleared
        bool valid = false;
        JobState state = JobState::Queued;
        QJsonObject details;
        QString summary;
        QStringList warnings;
        QString error;
    } m_compareResult;
    bool m_compareDemo = false;

    // convert
    QWidget* m_convertTilesHost = nullptr;
    ui::ChoiceTiles* m_convertTo = nullptr;
    QString m_convertFrom; // the format the tiles were made for
    ui::PathField* m_convertOutput = nullptr;
    QWidget* m_methodRow = nullptr;
    ui::Segmented* m_method = nullptr;
    QLabel* m_methodHelp = nullptr;

    // edit
    ui::ImageTreeReader* m_reader;
    ui::ImageTreeResult m_tree;
    QString m_treeFor; // the image m_tree / the pending read belongs to
    std::vector<StagedOp> m_ops;
    QTreeWidget* m_treeView = nullptr;
    ui::EmptyState* m_treeEmpty = nullptr;
    ui::ErrorState* m_treeError = nullptr;
    QLabel* m_treeCount = nullptr;
    QPushButton* m_btnAddFiles = nullptr;
    QPushButton* m_btnAddFolder = nullptr;
    ui::IconButton* m_btnMkdir = nullptr;
    ui::IconButton* m_btnReplace = nullptr;
    ui::IconButton* m_btnRename = nullptr;
    ui::IconButton* m_btnDelete = nullptr;
    ui::Card* m_stagedCard = nullptr;
    QVBoxLayout* m_stagedLayout = nullptr;
    QLabel* m_stagedCaption = nullptr;
    QPushButton* m_clear = nullptr;
};

} // namespace fw::pages
