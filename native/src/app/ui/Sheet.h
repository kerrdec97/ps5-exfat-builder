#pragma once

// In-window modal sheets (§4.17) and toasts (§4.16).
//
// A Sheet is a child of the main window: a `scrim` that covers everything and
// a 480/640 px panel in the middle. It runs its own event loop (exec()) so it
// behaves like a modal dialog, but it is part of the window, so screenshots
// capture it and the Windows title bar never appears on it.

#include "app/ui/Widgets.h"
#include "app/i18n/I18n.h"

#include <QCheckBox>
#include <QEventLoop>
#include <QPointer>
#include <QWidget>

namespace fw::ui {

class Sheet : public QWidget {
    Q_OBJECT
public:
    enum class Role { Safe, Primary, Destructive };
    enum class Icon { None, Info, Warning, Error };

    Sheet(QWidget* host, const QString& title, Icon icon = Icon::None, int width = theme::size::dialog);
    ~Sheet() override;

    QVBoxLayout* body() { return m_body; }
    QLabel* addText(const QString& text);        // body fg_1, wraps; supports rich text for mono spans
    QCheckBox* addCheckbox(const QString& text);
    QPushButton* addButton(const QString& text, Role role);
    void setTitle(const QString& title);

    // Shows the sheet and blocks until a button is pressed (returns its index),
    // Esc is pressed (returns the safe button's index, or -1), or close() is called.
    int exec();
    void open();                 // non-modal show (screenshots, countdowns)
    void close(int result = -1);
    bool isOpen() const { return isVisible(); }
    int result() const { return m_result; }

    // Convenience: a confirm dialog. Returns true when the action button was chosen.
    static bool confirm(QWidget* host, const QString& title, const QString& text, const QString& safeText,
                        const QString& actionText, bool destructive = true, Icon icon = Icon::Warning,
                        const QString& checkboxText = {}, bool* checkboxValue = nullptr);
    static void message(QWidget* host, const QString& title, const QString& text, Icon icon = Icon::Info,
                        const QString& buttonText = tr("OK"));
    static QString monoSpan(const QString& text);

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool focusNextPrevChild(bool next) override;

private:
    void relayout();
    QWidget* m_panel;
    QVBoxLayout* m_body;
    QHBoxLayout* m_buttons;
    QLabel* m_title;
    QList<QPushButton*> m_buttonList;
    QList<Role> m_roles;
    int m_width;
    int m_result = -1;
    QPointer<QEventLoop> m_loop;
    QPointer<QWidget> m_previousFocus;
};

// Bottom-right toast stack (max 3, newest at the bottom).
class ToastHost : public QWidget {
    Q_OBJECT
public:
    explicit ToastHost(QWidget* host);
    struct Action {
        QString text;
        std::function<void()> fn;
    };
    void show(Tone tone, const QString& title, const QString& body, const QList<Action>& actions = {},
              int lifetimeMs = -1); // -1: 6 s for Ok/Info, sticky for Warn/Err
    int count() const { return int(m_toasts.size()); }
    void clear();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void relayout();
    void remove(QWidget* toast);
    std::vector<QWidget*> m_toasts;
};

} // namespace fw::ui
