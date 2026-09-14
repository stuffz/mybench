#include "app/appstyle.h"

#include "ui/handcursor.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QWidget>

namespace
{

// The web build hand-rolled its tooltips because "native title tooltips are too
// slow to appear for icon-only destructive buttons" (index.css). The delay is a
// style hint rather than a property, so overriding it here restores that feel
// for every tooltip, item views included.
constexpr int TooltipWakeUpMs = 150;
constexpr int TooltipFallAsleepMs = 0;

} // namespace

void AppStyle::polish(QWidget *widget)
{
    QProxyStyle::polish(widget);
    if (!isClickable(widget))
    {
        return;
    }
    widget->setCursor(Qt::PointingHandCursor);

    // The popup is a window of its own, so it inherits nothing from the combo
    // it belongs to and its rows would keep the arrow. They are rows like any
    // other, hit test included. view() builds the popup here rather than on
    // the first click, which is the cost of reaching it from a style hook.
    if (auto *combo = qobject_cast<QComboBox *>(widget))
    {
        handCursorOnRows(combo->view());
    }
}

// A combo is not a button, but the whole closed control opens its popup, so it
// needs no hit test. Rows and tabs do, and live in ui/handcursor.h instead.
bool AppStyle::isClickable(const QWidget *widget)
{
    return qobject_cast<const QAbstractButton *>(widget) != nullptr ||
           qobject_cast<const QComboBox *>(widget) != nullptr;
}

void AppStyle::unpolish(QWidget *widget)
{
    if (isClickable(widget))
    {
        widget->unsetCursor();
    }
    QProxyStyle::unpolish(widget);
}

int AppStyle::styleHint(
    StyleHint hint, const QStyleOption *option, const QWidget *widget, QStyleHintReturn *returnData
) const
{
    switch (hint)
    {
    case SH_ToolTip_WakeUpDelay:
        return TooltipWakeUpMs;
    case SH_ToolTip_FallAsleepDelay:
        return TooltipFallAsleepMs;
    default:
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
}
