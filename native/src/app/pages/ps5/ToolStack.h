#pragma once

// A stack of pages that takes the size of the visible page only.
//
// QStackedWidget sizes itself (and, through height-for-width, its scroll
// area) to its largest page, so a short tool left the PS5 connect page
// scrolling into the empty space of the tallest one. Here the pages sit in a
// box layout and all but the current one are hidden; hidden widgets take no
// space.

#include <QVBoxLayout>
#include <QWidget>

#include <vector>

namespace fw::pages {

class ToolStack : public QWidget {
    Q_OBJECT
public:
    explicit ToolStack(QWidget* parent = nullptr) : QWidget(parent)
    {
        m_layout = new QVBoxLayout(this);
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setSpacing(0);
    }
    int addWidget(QWidget* w)
    {
        w->setParent(this);
        m_layout->addWidget(w, 1);
        m_pages.push_back(w);
        w->setVisible(int(m_pages.size()) - 1 == m_current || (m_current < 0 && m_pages.size() == 1));
        if (m_current < 0)
            m_current = 0;
        return int(m_pages.size()) - 1;
    }
    int count() const { return int(m_pages.size()); }
    QWidget* widget(int i) const { return i >= 0 && i < count() ? m_pages[size_t(i)] : nullptr; }
    QWidget* currentWidget() const { return widget(m_current); }
    int currentIndex() const { return m_current; }
    int indexOf(const QWidget* w) const
    {
        for (int i = 0; i < count(); ++i)
            if (m_pages[size_t(i)] == w)
                return i;
        return -1;
    }

public slots:
    void setCurrentIndex(int index)
    {
        if (index < 0 || index >= count() || index == m_current)
            return;
        m_current = index;
        for (int i = 0; i < count(); ++i)
            m_pages[size_t(i)]->setVisible(i == index);
        emit currentChanged(index);
    }

signals:
    void currentChanged(int index);

private:
    QVBoxLayout* m_layout;
    std::vector<QWidget*> m_pages;
    int m_current = -1;
};

} // namespace fw::pages
