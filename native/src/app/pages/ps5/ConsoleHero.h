#pragma once

// The console card at the top of PS5 connect: who the console is (address,
// FTP port, connection state), how to reach it (Find my PS5, Connect /
// Disconnect) and which services it runs, as chips:
//
//   FTP · ftpsrv v0.21.1   Kernel log · 3232   Payload loader · 9021
//   ShadowMount+ · 1.7beta11   Installer · 12800
//
// Each chip shows a state dot (colour + word in its tooltip) and opens the
// group that uses the service. The card lays its controls out in one row when
// there is room and in two rows on narrow windows.

#include "app/ui/Widgets.h"

#include <QAbstractButton>
#include <QHash>

class QGridLayout;

namespace fw {
class AppContext;
}

namespace fw::pages {

class FindConsoleButton;

// One service pill: dot, icon, name, detail.
class ServiceChip : public QAbstractButton {
    Q_OBJECT
public:
    ServiceChip(const QString& key, const QString& icon, const QString& name, QWidget* parent = nullptr);
    QString key() const { return m_key; }
    void setState(ui::Tone tone, const QString& detail, const QString& tooltip = {});
    ui::Tone tone() const { return m_tone; }
    QString detail() const { return m_detail; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    QString m_key, m_icon, m_name, m_detail;
    ui::Tone m_tone = ui::Tone::Neutral;
    bool m_hover = false;
};

class ConsoleHero : public QWidget {
    Q_OBJECT
public:
    enum class State { Offline, Connecting, Connected, Failed };
    Q_ENUM(State)

    ConsoleHero(AppContext& ctx, QWidget* parent = nullptr);
    QLineEdit* hostEdit() { return m_host; }
    QLineEdit* portEdit() { return m_port; }
    QPushButton* connectButton() { return m_connect; }
    FindConsoleButton* findButton() { return m_find; }
    ui::Badge* stateBadge() { return m_badge; }

    void setState(State state, const QString& detail = {});
    State state() const { return m_state; }
    void setSubtitle(const QString& text);
    QString subtitle() const;
    // Service chips by key: "ftp", "klog", "payload", "shadowmount", "installer".
    ServiceChip* chip(const QString& key) const { return m_chips.value(key); }
    void setChip(const QString& key, ui::Tone tone, const QString& detail, const QString& tooltip = {});
    void setChecking(bool on);
    QSize minimumSizeHint() const override;

signals:
    void chipClicked(const QString& key);
    void checkServicesRequested();
    void stateChanged(ConsoleHero::State state); // setState() was called

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void relayout();

    AppContext& m_ctx;
    State m_state = State::Offline;
    ui::IconTile* m_tile;
    QLabel* m_title;
    ui::Badge* m_badge;
    ui::ElideLabel* m_subtitle;
    QWidget* m_controls;
    QLabel* m_hostLabel;
    QLabel* m_portLabel;
    QLineEdit* m_host;
    QLineEdit* m_port;
    FindConsoleButton* m_find;
    QPushButton* m_connect;
    ui::IconButton* m_check;
    QGridLayout* m_grid;
    QWidget* m_identity;
    QWidget* m_chipRow;
    QHash<QString, ServiceChip*> m_chips;
    bool m_narrow = false;
};

} // namespace fw::pages
