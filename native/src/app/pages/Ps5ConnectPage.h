#pragma once

// PS5 connect (Connect artboard): console address + reachability probe,
// tabs FTP browser · Payloads · Kernel log. FTP and payloads submit through
// ps5.ftp.upload / ps5.payload.send; the browser and kernel log show the
// designed unavailable state while the connect module is absent.

#include "app/pages/Page.h"
#include "app/services/AppStores.h"

#include <QListWidget>
#include <QStackedWidget>
#include <QTabBar>
#include <QTcpSocket>

namespace fw::pages {

class Ps5ConnectPage : public Page {
    Q_OBJECT
public:
    Ps5ConnectPage(AppContext& ctx, JobQueueModel& queueModel, ui::PayloadStore& payloads, QWidget* parent = nullptr);
    void navigateWith(const QVariantMap& args) override;
    void refresh() override { probe(); }
    QString statusContext() const override;
    QString host() const;
    int ftpPort() const;
    int payloadPort() const;
    bool isReachable() const { return m_reachable; }
    bool acceptsDrop(const QList<QUrl>& urls) const override;
    void dropUrls(const QList<QUrl>& urls) override;
signals:
    void reachabilityChanged(bool reachable, const QString& host);

private:
    void probe();
    void setReachable(bool on, const QString& detail);
    void rebuildPayloads();
    void showPayload(int index);
    void addOrEditPayload(int index);
    void sendPayload(int index);
    void uploadFile();
    void bindSettingKeys();

    ui::PayloadStore& m_payloads;
    QLineEdit* m_host;
    QLineEdit* m_ftpPort;
    QLineEdit* m_payloadPort;
    QPushButton* m_test;
    ui::Badge* m_badge;
    QTcpSocket* m_socket = nullptr;
    bool m_reachable = false;
    QString m_probeDetail;
    QTabBar* m_tabs;
    QStackedWidget* m_stack;
    // FTP
    ui::PathField* m_uploadLocal;
    QLineEdit* m_uploadRemote;
    QPushButton* m_uploadButton;
    ui::UnavailableState* m_ftpUnavailable;
    // Payloads
    QListWidget* m_payloadList;
    ui::Card* m_payloadDetail;
    QLabel* m_pName;
    ui::Badge* m_pBadge;
    QLabel* m_pFile;
    QLabel* m_pNotes;
    QLabel* m_pLastSent;
    QPushButton* m_pSend;
    QPushButton* m_pEdit;
    QPushButton* m_pRemove;
    ui::EmptyState* m_payloadEmpty;
    int m_currentPayload = -1;
    QString m_hostKey, m_ftpPortKey, m_payloadPortKey;
};

} // namespace fw::pages
