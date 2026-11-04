#pragma once

// Extract & verify: source image or .pkg · destination · mode (Extract and
// verify / Verify only) · job settings · Jobs dock.

#include "app/pages/Page.h"

namespace fw::pages {

class ExtractVerifyPage : public JobFormPage {
    Q_OBJECT
public:
    ExtractVerifyPage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent = nullptr);
    bool acceptsDrop(const QList<QUrl>& urls) const override;
    void dropUrls(const QList<QUrl>& urls) override;
    void navigateWith(const QVariantMap& args) override;
    void setSource(const QString& file);
    bool verifyOnly() const;

protected:
    Result<QString> submit(bool buildNow) override;
    QString validateForm() override;

private:
    void updateKinds();
    ui::PathField* m_source;
    ui::Badge* m_sourceBadge;
    QLabel* m_detected;
    ui::Segmented* m_mode;
    ui::PathField* m_destination;
    QWidget* m_destinationField;
    QLabel* m_destHelp;
};

} // namespace fw::pages
