#pragma once

// Extract & verify: source image or .pkg · destination · mode (Extract and
// verify / Verify only) · job settings · Jobs dock; and the Compare card:
// image A against image or folder B (image.compare), with the result sheet
// (CompareResultSheet) read from the job's outcome.

#include "app/pages/Page.h"

#include <QJsonObject>
#include <QPointer>

namespace fw::pages {

class CompareResultSheet;

class ExtractVerifyPage : public JobFormPage {
    Q_OBJECT
public:
    ExtractVerifyPage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent = nullptr);
    bool acceptsDrop(const QList<QUrl>& urls) const override;
    void dropUrls(const QList<QUrl>& urls) override;
    void navigateWith(const QVariantMap& args) override;
    void setSource(const QString& file);
    bool verifyOnly() const;
    QStringList demoStates() const override;
    void applyDemoState(const QString& name) override;
    void clearDemoState() override;

    // Compare card. `b` may be an image or a folder; depth "full" | "structure".
    void setCompareInputs(const QString& a, const QString& b, const QString& depth = QStringLiteral("full"));
    void startCompare();
    QString compareJobId() const { return m_compareJobId; }
    QString compareStatus() const; // the card's badge text
    QPushButton* compareButton() { return m_compareButton; }
    QPushButton* compareResultButton() { return m_compareView; }
    // Opens the result of the last comparison (non-modal); nullptr when there is none.
    CompareResultSheet* showCompareResult();

protected:
    Result<QString> submit(bool buildNow) override;
    QString validateForm() override;

private:
    void updateKinds();
    void buildCompareCard();
    void updateCompareKind();
    void onCompareJob(const QString& id, bool finished);
    void showCompareState();

    ui::PathField* m_source;
    ui::Badge* m_sourceBadge;
    QLabel* m_detected;
    ui::Segmented* m_mode;
    ui::PathField* m_destination;
    QWidget* m_destinationField;
    QLabel* m_destHelp;

    ui::Card* m_compareCard = nullptr;
    ui::PathField* m_compareA = nullptr;
    ui::Segmented* m_compareBKind = nullptr;
    ui::PathField* m_compareB = nullptr;
    ui::Segmented* m_compareDepth = nullptr;
    ui::Badge* m_compareBadge = nullptr;
    QLabel* m_compareSummary = nullptr;
    ui::MessageRow* m_compareMessage = nullptr;
    QPushButton* m_compareView = nullptr;
    QPushButton* m_compareButton = nullptr;
    QString m_compareJobId;
    // The last finished comparison, kept when its queue entry is cleared.
    struct CompareResult {
        bool valid = false;
        JobState state = JobState::Queued;
        QJsonObject details;
        QString summary;
        QStringList warnings;
        QString error;
    } m_compareResult;
    QPointer<QWidget> m_demoSheet;
    bool m_compareDemo = false;
};

} // namespace fw::pages
