#pragma once
// The pill-and-knob switch the settings rows toggle with.
#include <QCheckBox>

// The web dialog uses switches, not checkboxes. A QCheckBox that paints
// itself as a pill-and-knob switch keeps all the checkable behaviour and
// signal wiring; only the look changes. Colours are read per paint, so a
// theme switch repaints it for free.
class SwitchBox : public QCheckBox
{
public:
    // With text, the label is painted after the pill and clicks anywhere on
    // the widget toggle — the toolbar-checkbox replacement.
    explicit SwitchBox(const QString &text = {});

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    // QCheckBox only accepts clicks inside the style's standard indicator
    // rect — a 13px square that covers a corner of the painted pill. The
    // whole switch is the control; the whole switch must toggle it.
    bool hitButton(const QPoint &pos) const override;

    void paintEvent(QPaintEvent *event) override;
};
