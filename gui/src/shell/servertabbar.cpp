#include "shell/servertabbar.h"

#include "app/icons.h"
#include "app/theme.h"
#include "ui/closeglyph.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

ServerTabBar::ServerTabBar(QWidget *parent) : QWidget(parent)
{
    const AppPalette &pal = theme::current();
    // Without this a plain-QWidget subclass never paints its stylesheet box,
    // so the border below would silently not exist.
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QString("ServerTabBar { border-bottom: 1px solid %1; }").arg(pal.border.name()));
    m_row = new QHBoxLayout(this);
    m_row->setContentsMargins(8, 6, 8, 0);
    m_row->setSpacing(4);
    m_row->addStretch();
    setVisible(false);
}

void ServerTabBar::applyTheme()
{
    // The bar's own separator too: the constructor ran under the startup
    // theme, and a stale sheet would keep that theme's border colour.
    setStyleSheet(
        QString("ServerTabBar { border-bottom: 1px solid %1; }").arg(theme::current().border.name())
    );
    // Every tab carries its own stylesheet (accent bar, active/inactive fill),
    // so the row has to be rebuilt from the inputs it was last given.
    setConnections(m_conns, m_colors, m_active);
}

void ServerTabBar::setConnections(
    const QVector<QPair<QString, QString>> &conns, const QHash<QString, QColor> &colors,
    const QString &active
)
{
    m_conns = conns;
    m_colors = colors;
    m_active = active;
    // Rebuild wholesale: the row is at most a handful of tabs.
    while (QLayoutItem *item = m_row->takeAt(0))
    {
        if (QWidget *w = item->widget())
        {
            w->deleteLater();
        }
        delete item;
    }
    const AppPalette &pal = theme::current();

    for (const auto &c : conns)
    {
        const QString id = c.first;
        const QColor accent = colors.value(id, pal.info);
        const bool isActive = id == active;

        auto *tab = new QWidget(this);
        auto *lay = new QHBoxLayout(tab);
        lay->setContentsMargins(10, 4, 6, 4);
        lay->setSpacing(6);
        tab->setCursor(Qt::PointingHandCursor);
        // Accent bar across the top, dimmed when the tab is not active; the
        // active tab merges into the row below by dropping its bottom border.
        QColor top = accent;
        if (!isActive)
        {
            top.setAlpha(102);
        }
        tab->setStyleSheet(QString("QWidget { background: %1; border-top: 2px solid %2; "
                                   "border-left: 1px solid %3; border-right: 1px solid %3; }")
                               .arg(
                                   isActive ? pal.background.name() : "transparent",
                                   top.name(QColor::HexArgb),
                                   isActive ? pal.border.name() : "transparent"
                               ));

        auto *dot = new QLabel;
        dot->setFixedSize(8, 8);
        dot->setStyleSheet(QString("QLabel { background: %1; border-radius: 4px; border: none; }")
                               .arg(accent.name()));
        lay->addWidget(dot);

        auto *name = new QLabel(c.second);
        name->setStyleSheet(QString("QLabel { border: none; color: %1; }")
                                .arg(isActive ? pal.foreground.name() : pal.mutedFg.name()));
        lay->addWidget(name);

        auto *close = new QPushButton;
        close->setIcon(icons::icon("x", pal.mutedFg, 11));
        close->setToolTip(tr("Disconnect"));
        close->setFixedSize(16, 16);
        close->setStyleSheet(QString("QPushButton { border: none; background: transparent; "
                                     "color: %1; padding: 0; font-size: %3px; }"
                                     "QPushButton:hover { color: %2; }")
                                 .arg(pal.mutedFg.name(), pal.foreground.name())
                                 .arg(theme::scaledPx(CloseGlyphScale)));
        connect(close, &QPushButton::clicked, this, [this, id]() { emit closeRequested(id); });
        lay->addWidget(close);

        // Clicking anywhere on the tab activates it.
        name->installEventFilter(this);
        tab->installEventFilter(this);
        tab->setProperty("connID", id);
        name->setProperty("connID", id);
        dot->setProperty("connID", id);

        m_row->addWidget(tab);
    }
    m_row->addStretch();
    setVisible(!conns.isEmpty());
}

bool ServerTabBar::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress)
    {
        const QString id = watched->property("connID").toString();
        if (!id.isEmpty())
        {
            emit activated(id);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
