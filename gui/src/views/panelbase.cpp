#include "views/panelbase.h"

#include "app/theme.h"
#include "ui/widgets.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

PanelBase::PanelBase(const QString &title, const QString &connID, QWidget *parent)
    : QWidget(parent), m_connID(connID)
{
    m_root = new QVBoxLayout(this);
    m_root->setContentsMargins(0, 0, 0, 0);
    m_root->setSpacing(0);

    // --- title row: name, Refresh, then whatever the subclass adds ---------
    // (addFilter/addHeaderWidget append here; setBody caps it with a stretch)
    auto *headerWrap = new QWidget;
    headerWrap->setObjectName("PanelHeader"); // border-bottom rule in theme.cpp
    headerWrap->setAttribute(Qt::WA_StyledBackground, true);
    m_header = new QHBoxLayout(headerWrap);
    m_header->setContentsMargins(12, 8, 12, 8);
    m_header->setSpacing(8);

    auto *lbl = weightedLabel(title, QFont::Medium);

    auto *refreshBtn = new QPushButton(tr("Refresh"));
    connect(refreshBtn, &QPushButton::clicked, this, [this]() { refresh(); });

    m_header->addWidget(lbl);
    m_header->addWidget(refreshBtn);
    m_root->addWidget(headerWrap);

    // --- error strip under the title row, hidden until showError() ---------
    m_error = new QLabel;
    m_error->setVisible(false);
    m_error->setWordWrap(true);
    m_root->addWidget(m_error);

    // Re-styled on theme change: a stylesheet baked at construction would keep
    // the old theme's error colour after a light/dark switch.
    const auto restyleError = [this]()
    {
        m_error->setStyleSheet(QString("QLabel { color: %1; padding: 4px 12px; }")
                                   .arg(theme::current().destructive.name()));
    };
    restyleError();
    connect(theme::notifier(), &Notifier::changed, this, restyleError);
}

QLineEdit *PanelBase::addFilter(const QString &placeholder)
{
    auto *e = new QLineEdit;
    e->setPlaceholderText(placeholder);
    e->setClearButtonEnabled(true);
    e->setMinimumWidth(220);
    m_header->addWidget(e);
    return e;
}

void PanelBase::addHeaderWidget(QWidget *w)
{
    m_header->addWidget(w);
}

void PanelBase::setBody(QWidget *w)
{
    m_header->addStretch();
    m_root->addWidget(w, 1);
}

void PanelBase::showError(const QString &message)
{
    m_error->setText(message);
    m_error->setVisible(!message.isEmpty());
}
