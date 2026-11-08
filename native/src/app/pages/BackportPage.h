#pragma once

// Backport — the owner's PS5 Backport GUI 0.3.4, built into the app.
//
// The page is a native, v5-styled replacement for that program's window and
// behaves one-to-one: the same scan rules, options, dialogs, texts, run order
// and results. Every build, SDK rebuild, PS5 Pro build, AMPR / PlayGo switch
// and restore runs in the bundled PS5 Backport engine (app/backport/
// BackportHost.h), so outputs are byte-identical to the standalone program.
//
// Layout: Game library (scan folders, search, tick games) · Progress and
// Activity on the left; Save to (backport ZIPs, originals backup ZIPs),
// Options (Target SDK, Apply, PS5 Pro fix, PS5 Pro build), the black-screen
// tip and Backups & support on the right; Start Backport / Cancel and
// "Switch AMPR / PlayGo…" in the page header.

#include "app/backport/BackportCore.h"
#include "app/backport/BackportHost.h"
#include "app/pages/GameTable.h" // the Library page's table (moved out of this page)
#include "app/pages/Page.h"
#include "app/services/AppStores.h"
#include "app/ui/Components.h"

#include <QAbstractTableModel>
#include <QJsonObject>
#include <QPointer>
#include <QSortFilterProxyModel>

#include <atomic>
#include <functional>
#include <memory>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QTableView;
class QTextEdit;

namespace fw {
class JobQueueModel;
class LogModel;
} // namespace fw

namespace fw::pages {

// ---- game list model -----------------------------------------------------------------

struct BackportRow {
    bp::Game game;
    bool checked = true;
    QString status = QStringLiteral("Ready");
    QString tone = QStringLiteral("idle"); // idle info warn muted busy good bad
    QString detail;
    QJsonObject result;
    int pct = 0;
};

class BackportGameModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { SelectCol, CoverCol, TitleCol, PpsaCol, SdkCol, FwCol, StatusCol, ColumnCount };
    enum Role { RowRole = Qt::UserRole + 1, SortRole, FilterRole };

    explicit BackportGameModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;

    std::vector<BackportRow>& rows() { return m_rows; }
    const std::vector<BackportRow>& rows() const { return m_rows; }
    int indexOfKey(const QString& key) const;
    BackportRow* rowByKey(const QString& key);
    void clear();
    void append(BackportRow row);
    void removeKey(const QString& key);
    void rowChanged(const QString& key);
    void allChanged();
    void setLocked(bool locked) { m_locked = locked; }

signals:
    void checkedChanged();

private:
    std::vector<BackportRow> m_rows;
    bool m_locked = false;
};

class BackportFilterModel : public QSortFilterProxyModel {
    Q_OBJECT
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void setSearch(const QString& text);

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override;
    bool lessThan(const QModelIndex& a, const QModelIndex& b) const override;

private:
    QString m_search;
};

// ---- page ----------------------------------------------------------------------------

class BackportPage : public Page {
    Q_OBJECT
public:
    BackportPage(AppContext& ctx, JobQueueModel& queueModel, LogModel& logModel, ui::LibraryStore& library,
                 QWidget* parent = nullptr);
    ~BackportPage() override;

    bool hasPrimaryAction() const override { return true; }
    void primaryAction() override { startOrCancel(); }
    void refresh() override { rescan(); }
    bool acceptsDrop(const QList<QUrl>& urls) const override;
    void dropUrls(const QList<QUrl>& urls) override;
    void navigateWith(const QVariantMap& args) override;
    QString statusContext() const override;
    void setDragHighlight(bool on) override;
    void onShown() override;
    QStringList demoStates() const override;
    void applyDemoState(const QString& name) override;
    void clearDemoState() override;

    // Close handling (MainWindow): a run or restore in progress.
    bool isBusy() const { return isRunning(); }
    // Cancels, then pumps events until the current worker exits (at most `ms`).
    void cancelAndWait(int ms);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum class Kind { Backport, Sdk, ProBuild, Fakelib, Restore };
    struct Item {
        QJsonObject job;
        QString rowKey; // empty for a chosen file
        QString name;   // for labels and the summary
    };
    enum class Engine { Unknown, Preparing, Ready, Missing, Failed };

    // layout
    void buildLibraryCard();
    void buildProgressCard();
    void buildSideColumn(QVBoxLayout* into);
    void updateLibraryState();
    void updateStats();
    void updateModeLine();
    void updateButtons();
    void setState(const QString& text, ui::Tone tone);

    // engine
    void prepareEngine();
    void engineReady();
    void offerInterruptedRestores();

    // scanning
    void addRoots(const QStringList& dirs);
    void rescan();
    void onGameScanned(quint64 gen, const bp::Game& g);
    void onScanFinished(quint64 gen, int count);
    void editScanLocations();
    void clearList();

    // runs
    void startOrCancel();
    void startRun(const QStringList& keys);
    void beginRun(const QStringList& keys);
    void requestCancel();
    void runNext();
    void onProgress(const QJsonObject& ev);
    void onFinished(const QJsonObject& result);
    void rowResult(BackportRow& row, const QJsonObject& r);
    void runFinished();
    void startBatch(Kind kind, std::vector<Item> items);
    void batchFinished();
    void retryFailed();

    // row menu and dialogs
    void showRowMenu(const QPoint& globalPos);
    void showDetails(const QString& key);
    void rebuildWithSdk(const QStringList& keys);
    void makeProBuild(const QStringList& keys);
    void switchFakelib(const QStringList& keys);
    bool chooseFakelib(const QStringList& names, QJsonValue* ampr, QJsonValue* playgo);
    QStringList chooseBackportFiles(const QString& question, const QString& fileTitle, bool* cancelled);
    void pickRestore();
    void runRestore(const QString& backupRun);
    void exportLogs(const QString& note, bool reveal);
    void reportProblem();
    void openPath(const QString& path);
    void saveDestinations();

    // activity log
    void log(const QString& level, const QString& message, bool toBox = true);
    static bool testMode();
    bool persistAllowed() const;
    void persistSettings();

    QStringList selectedKeys() const; // highlighted rows (view selection)
    QStringList checkedKeys() const;
    BackportRow* currentRow();
    int targetSdk() const;
    QString outputRoot() const;

    // ---- state
    bp::Settings m_settings;
    BackportGameModel* m_model;
    BackportFilterModel* m_proxy;
    Engine m_engine = Engine::Unknown;
    QString m_engineRevision, m_engineError;
    QPointer<bp::Worker> m_worker;
    // The page's run lifecycle; setRunState() is its only writer:
    //   Idle ─▶ Scanning ─▶ Idle                       (rescan / clearList)
    //   Idle ─▶ Running ─▶ Cancelling ─▶ Idle           (a Restore cannot be cancelled)
    enum class RunState { Idle, Scanning, Running, Cancelling };
    RunState m_runState = RunState::Idle;
    void setRunState(RunState state);
    bool isRunning() const { return m_runState == RunState::Running || m_runState == RunState::Cancelling; }
    bool isCancelling() const { return m_runState == RunState::Cancelling; }
    bool isScanning() const { return m_runState == RunState::Scanning; }
    quint64 m_scanGen = 0;
    std::shared_ptr<std::atomic_bool> m_scanCancel;
    Kind m_kind = Kind::Backport;
    QStringList m_runKeys;
    int m_runIndex = 0;
    QString m_runRowKey;
    std::vector<Item> m_items;
    std::vector<std::pair<Item, QJsonObject>> m_batchResults;
    QJsonObject m_runOptions;
    QString m_lastOutputRoot;
    QString m_lastLabel;
    int m_sdkPair = 5;
    bool m_shownOnce = false;
    bool m_demo = false;
    bool m_suppressSave = false;
    QString m_rowSavedStatus, m_rowSavedTone, m_rowSavedDetail;

    // ---- widgets
    QPushButton* m_start;
    QPushButton* m_switchBtn;
    ui::Card* m_libraryCard;
    ui::Card* m_progressCard;
    ui::Card* m_activityCard;
    ui::Badge* m_countBadge;
    QLineEdit* m_search;
    QPushButton* m_rootsBtn;
    ui::IconButton* m_rescanBtn;
    QPushButton* m_addBtn;
    QLabel* m_scanLabel;
    QTableView* m_table;
    ui::DropZone* m_emptyDrop;
    QLabel* m_emptyText;
    ui::ProgressBar* m_bar;
    QLabel* m_percent;
    QLabel* m_current;
    QLabel* m_stage;
    QHash<QString, QLabel*> m_chips;
    QLabel* m_modeLine;
    QTextEdit* m_logBox;
    QPushButton* m_openOutput;
    QPushButton* m_retry;
    ui::PathField* m_outField;
    ui::PathField* m_bakField;
    QComboBox* m_sdk;
    QLabel* m_sdkCaption;
    ui::Switch* m_apply;
    ui::Switch* m_proFix;
    ui::Switch* m_proBuild;
    QSpinBox* m_depth;
    ui::Badge* m_engineBadge;
    QLabel* m_engineLine;
    ui::InfoBar* m_engineBar;
};

} // namespace fw::pages
