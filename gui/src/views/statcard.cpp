#include "views/statcard.h"

#include "app/icons.h"
#include "app/theme.h"
#include "ui/widgets.h"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QVBoxLayout>

#include <algorithm>

StatCard::StatCard(const QString &icon, const QString &label, QWidget *parent)
    : QWidget(parent), m_iconName(icon)
{
    // A plain QWidget ignores a stylesheet background and border without
    // this; only QFrame-derived widgets paint them by default.
    setAttribute(Qt::WA_StyledBackground, true);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 9, 12, 10);
    root->setSpacing(1);

    auto *top = new QHBoxLayout;
    top->setSpacing(6);
    m_icon = new QLabel;
    m_label = mutedLabel(label);
    top->addWidget(m_icon);
    top->addWidget(m_label);
    top->addStretch();
    root->addLayout(top);

    // Sizes come from the stylesheet (see theme.cpp): the global QWidget rule
    // overrides setFont, so these object names are what actually make the
    // value large and the sub-line small — and keeps both on the prefs slider.
    m_value = new QLabel("—");
    m_value->setObjectName("kpiValue");
    root->addWidget(m_value);

    m_sub = mutedLabel();
    m_sub->setObjectName("kpiSub");
    root->addWidget(m_sub);

    applyTheme();
}

void StatCard::setValue(const QString &value, const QString &sub)
{
    m_valueText = value;
    m_subText = sub;
    elide();
}

void StatCard::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    elide();
}

void StatCard::elide()
{
    const int room = std::max(40, width() - 24);
    m_value->setText(QFontMetrics(m_value->font()).elidedText(m_valueText, Qt::ElideRight, room));
    m_sub->setText(QFontMetrics(m_sub->font()).elidedText(m_subText, Qt::ElideRight, room));
    // The tile is narrow by design; the full reading lives in the tooltip.
    const QString full = m_valueText + (m_subText.isEmpty() ? QString() : " — " + m_subText);
    m_sub->setToolTip(m_subText);
    m_value->setToolTip(full);
}

void StatCard::setTone(const QColor &tone)
{
    m_tone = tone;
    m_value->setStyleSheet(tone.isValid() ? QString("color: %1;").arg(tone.name()) : QString());
}

void StatCard::applyTheme()
{
    const AppPalette &pal = theme::current();
    setStyleSheet(QString("StatCard { background: %1; border: 1px solid %2; border-radius: 8px; }")
                      .arg(pal.card.name(), pal.border.name()));
    m_icon->setPixmap(icons::pixmap(m_iconName, pal.mutedFg, 14));
}
