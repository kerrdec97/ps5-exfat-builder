#pragma once

// Build (Main artboard): one page for every output the app makes.
//
//   Source       drop zone → game strip. A folder for every format; with FPKG
//                also an .exfat / .ffpfsc / .ffpkg image or a .gp5 project.
//                A wrapper folder is searched for the game folder below it.
//   Format       four tiles (exFAT, FFPKG, PFS, FPKG) and, under them, the two
//                or three settings people change for that format.
//   Package      FPKG only: package type (App, Add-on, Update, Backport),
//                content ID, title, version, add-on / update fields, and the
//                backport updates (4.xx / 5.xx+ backport files, folder or ZIP):
//                with App they are built as separate updates next to the new
//                package; Backport builds them against an existing base .pkg.
//   Save to      output folder and the file name (editable; preset from
//                Settings › Build › Output name).
//   Advanced     one folded section with the rest of that format's per-job
//                settings and "Verification & after the build".
//
// Each format has its own JobOptionsPanel (key filter per format); its rows
// are placed in the Format card or the Advanced section, so a format never
// shows another format's options. submit() sends the current format's
// overrides only.

#include "app/pages/Page.h"
#include "app/services/GameInfo.h"
#include "app/ui/Components.h"
#include "core/FileUtil.h"

#include <QComboBox>
#include <QHash>
#include <QPointer>
#include <QSet>

#include "core/Cancellation.h"

#include <memory>

class QMenu;

namespace fw::pages {

// Format values of the page (navigateWith {"format": ...}); images use the
// jobs::kFormat* values ("exfat", "ffpkg", "ffpfsc", "ffpfs").
inline const QString kFormatFpkg = QStringLiteral("fpkg");

class BuildImagePage : public JobFormPage {
    Q_OBJECT
public:
    BuildImagePage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent = nullptr);
    bool acceptsDrop(const QList<QUrl>& urls) const override;
    void dropUrls(const QList<QUrl>& urls) override;
    void navigateWith(const QVariantMap& args) override;
    void refresh() override;
    void onShown() override;
    QString statusContext() const override;
    QStringList demoStates() const override;
    void applyDemoState(const QString& name) override;
    void clearDemoState() override;
    void setDragHighlight(bool on) override;

    void setSource(const QString& path);
    QString source() const;
    // "exfat", "ffpkg", "ffpfsc", "ffpfs" (PFS tile, uncompressed) or "fpkg".
    void setFormat(const QString& format);
    QString format() const;
    // Image formats: the file name with its extension; FPKG: the expected package name.
    QString fileName() const;
    void setFileStem(const QString& stem); // as if typed by the user
    void setOutputFolder(const QString& dir); // as if chosen with Browse…
    bool fileNameEdited() const { return m_nameEdited; }
    void showSpaceError(qint64 needed, qint64 free, const QString& drive); // real flow + demo state

    // Introspection for tests and screenshots.
    ui::JobOptionsPanel* optionsPanel(const QString& format) const; // panel of a tile ("ffpfs" → the PFS panel)
    QStringList visibleOptionKeys() const; // setting rows of the current format that are not hidden
    QStringList essentialOptionKeys() const; // the rows shown in the Format card
    ui::Disclosure* advancedSection() const { return m_advanced; }
    const ui::GameSummary& summary() const { return m_summary; }
    QWidget* packageCard() const { return m_packageCard; }
    QString sourceMessage() const;
    QStringList gameRootChoices() const { return m_rootChoices; }

protected:
    Result<QString> submit(bool buildNow) override;
    QString validateForm() override;

private:
    struct FormatOptions {
        QString tile; // exfat / ffpkg / ffpfsc / fpkg
        ui::JobOptionsPanel* panel = nullptr;
        QWidget* essentials = nullptr; // in the Format card
        QWidget* advanced = nullptr;   // in the Advanced section
        QStringList essentialKeys, advancedKeys;
        QHash<QString, QWidget*> headings; // bucket title → heading label (advanced)
        QHash<QString, QString> bucketOf;  // key → bucket title
    };
    void buildSourceCard();
    void buildFormatCard();
    void buildPackageCard();
    void buildSaveCard();
    void buildAdvanced();
    void buildSummaryCard();
    void buildFormatOptions(FormatOptions& fo, const QStringList& sections, const QStringList& keyFilter,
                            const QStringList& exclude, const QStringList& essentials);
    void onFormatChanged();
    void applyRowVisibility();
    void updateAdvancedSummary();
    void updateClusterNote();
    void updatePfsRows();
    void browseSource();
    void onSummary(const ui::GameSummary& s);
    void refreshSourceState(); // strip, badge and messages for m_summary and the format
    void showGameRoots(const ui::GameSummary& s);
    void updateOutput();
    void renderFileName();
    void checkSpace();
    void updateToolsStatus();
    void applyLiveCpuLimit();
    void pickFromDlcEmu();
    void onPackageTypeChanged();
    enum class PackageType { App, Addon, Update, Backport };
    QString packageType() const;   // the tile's value: app / addon / update / backport
    PackageType packageTypeKind() const;
    bool hasBackports() const;     // a 4.xx or 5.xx+ backport is chosen
    void inspectBase();            // reads the base .pkg (title, version) for Backport
    void browseBackport(ui::PathField* field, const QString& label);
    qint64 neededBytes() const;
    QString tile() const;
    bool isFpkg() const { return tile() == kFormatFpkg; }
    QString sourceKind() const; // folder / exfat / ffpfsc / ffpkg / gp5
    QString outputFolder() const; // where the output goes (resolved defaults)
    QString expectedPackagePath() const;
    FormatOptions* current();
    const FormatOptions* current() const;

    ui::GameInfoReader* m_reader = nullptr;
    ui::GameSummary m_summary;
    QString m_sourcePath;
    QString m_sourceKind;              // sourceKindOf(m_sourcePath), decided when the source is set (never a stat per repaint)
    QSet<QString> m_liveLimitApplied;  // package builds whose live CPU limit was applied on start
    ui::Card* m_sourceCard = nullptr;
    ui::Badge* m_sourceBadge = nullptr;
    QWidget* m_strip = nullptr;
    ui::ElideLabel* m_stripTitle = nullptr;
    ui::ElideLabel* m_stripMeta = nullptr;
    ui::ElideLabel* m_stripPath = nullptr;
    ui::IconTile* m_stripTile = nullptr;
    ui::DropZone* m_drop = nullptr;
    ui::MessageRow* m_sourceMsg = nullptr;
    QWidget* m_rootsBox = nullptr;
    QVBoxLayout* m_rootsList = nullptr;
    QStringList m_rootChoices;
    QString m_pendingRootNote;

    QWidget* m_formatTiles = nullptr; // FormatTiles (BuildImagePage.cpp)
    QWidget* m_essentialsHost = nullptr;
    ui::Segmented* m_pfsKind = nullptr;
    QWidget* m_pfsKindRow = nullptr;
    QWidget* m_toolsRow = nullptr;
    ui::Badge* m_toolsBadge = nullptr;
    QLabel* m_toolsDetail = nullptr;
    ui::Link* m_toolsLink = nullptr;
    std::vector<FormatOptions> m_formats;

    ui::Card* m_packageCard = nullptr;
    QLineEdit* m_contentId = nullptr;
    QLineEdit* m_title = nullptr;
    QLineEdit* m_version = nullptr;
    ui::Segmented* m_packageType = nullptr;
    QWidget* m_baseField = nullptr;
    ui::PathField* m_basePackage = nullptr;
    QWidget* m_addonField = nullptr;
    QWidget* m_idRow = nullptr;          // Content ID + version
    QWidget* m_titleRow = nullptr;
    QWidget* m_backportField = nullptr;  // the two backport pickers + note
    ui::PathField* m_backport4 = nullptr;
    ui::PathField* m_backport5 = nullptr;
    QLabel* m_backportNote = nullptr;
    ui::MessageRow* m_baseInfo = nullptr;
    QString m_baseTitleId;               // from the inspected base package
    QString m_autoVersion;               // the version this page filled in
    quint64 m_baseGen = 0;
    QString m_inspectedBase;             // the base path last inspected
    std::shared_ptr<CancellationSource> m_baseCancel;
    QLineEdit* m_entitlementKey = nullptr;
    QComboBox* m_volumeType = nullptr;

    ui::PathField* m_output = nullptr;
    ui::MessageRow* m_spaceRow = nullptr;
    ui::InfoBar* m_spaceBar = nullptr;
    QLabel* m_fileLabel = nullptr;
    QLineEdit* m_fileName = nullptr;
    QLabel* m_fileExt = nullptr;
    ui::IconButton* m_nameMenuButton = nullptr;
    ui::Link* m_resetName = nullptr;
    bool m_nameEdited = false;

    ui::Disclosure* m_advanced = nullptr;
    QWidget* m_advancedHost = nullptr;
    ui::Switch* m_openFolder = nullptr;
    ui::Link* m_settingsLink = nullptr;

    ui::Card* m_summaryCard = nullptr;
    QLabel* m_estSize = nullptr;
    QLabel* m_freeSpace = nullptr;
    QComboBox* m_liveCores = nullptr;
    QLabel* m_liveNote = nullptr;

    // The free-space check of the output folder; validateForm() refuses to
    // start on Insufficient and Unavailable and finishes a Checking one first.
    enum class SpaceCheck { Unknown, Checking, Ok, Warn, Insufficient, Unavailable };
    SpaceCheck m_spaceState = SpaceCheck::Unknown;
    void applySpaceCheck(const Result<DiskSpace>& ds, const Result<QStringList>& vol, const QString& folder, qint64 needed, bool pkg, bool exists);
    quint64 m_spaceGen = 0;
    bool m_spaceOk = true;
    QString m_spaceProblem;
    bool m_demo = false;
};

} // namespace fw::pages
