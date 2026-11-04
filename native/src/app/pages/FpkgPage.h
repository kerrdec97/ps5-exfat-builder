#pragma once

// FPKG (Fpkg artboard): Source · Package details · schema-driven option cards
// (FPKG, PlayGo, Verification, Performance) · Dependencies · Build summary ·
// Jobs dock.

#include "app/pages/Page.h"
#include "app/services/GameInfo.h"
#include "app/ui/DependencyRow.h"

#include <QCheckBox>

namespace fw::pages {

class FpkgPage : public JobFormPage {
    Q_OBJECT
public:
    FpkgPage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent = nullptr);
    bool acceptsDrop(const QList<QUrl>& urls) const override;
    void dropUrls(const QList<QUrl>& urls) override;
    void navigateWith(const QVariantMap& args) override;
    void refresh() override;
    QString statusContext() const override;
    void setSource(const QString& path);

protected:
    Result<QString> submit(bool buildNow) override;
    QString validateForm() override;

private:
    void onSourceTypeChanged();
    void onSummary(const ui::GameSummary& s);
    void updateOutput();
    void checkSpace();
    QString sourceType() const;

    ui::GameInfoReader* m_reader;
    ui::GameSummary m_summary;
    ui::Segmented* m_sourceType;
    ui::PathField* m_source;
    QLabel* m_sourceHelp;
    ui::PathField* m_output;
    QCheckBox* m_nextToSource;
    ui::MessageRow* m_spaceRow;
    QLineEdit* m_contentId;
    QLineEdit* m_title;
    QLineEdit* m_version;
    ui::Segmented* m_packageType;
    QWidget* m_baseField;
    ui::PathField* m_basePackage;
    ui::JobOptionsPanel* m_cards;
    ui::DependencyList* m_deps;
    ui::Badge* m_depsBadge;
    QLabel* m_estSize;
    QLabel* m_freeSpace;
    quint64 m_spaceGen = 0;
    bool m_spaceOk = true;
    QString m_spaceProblem;
};

} // namespace fw::pages
