#include "ui/widgets.h"

#include "app/theme.h"

#include <QFrame>
#include <QLabel>

QLabel *mutedLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setProperty("muted", true);
    return l;
}

QLabel *smallLabel(const QString &text)
{
    QLabel *l = mutedLabel(text);
    l->setObjectName("smallText");
    return l;
}

QLabel *weightedLabel(const QString &text, QFont::Weight weight)
{
    auto *l = new QLabel(text);
    QFont weighted = l->font();
    weighted.setWeight(weight);
    l->setFont(weighted);
    return l;
}

QFrame *hairline()
{
    auto *rule = new QFrame;
    rule->setFrameShape(QFrame::HLine);
    rule->setFixedHeight(1);
    rule->setStyleSheet(
        QString("QFrame { background: %1; border: none; }").arg(theme::current().border.name())
    );
    return rule;
}

QFrame *vhairline()
{
    auto *rule = new QFrame;
    rule->setFrameShape(QFrame::VLine);
    rule->setFixedWidth(1);
    rule->setStyleSheet(
        QString("QFrame { background: %1; border: none; }").arg(theme::current().border.name())
    );
    return rule;
}
