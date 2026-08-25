#include "ui/switchbox.h"

#include "app/theme.h"

#include <QFontMetrics>
#include <QPainter>

namespace
{

// Gap between the knob and the pill's edge — also what makes the knob
// smaller than the track.
constexpr qreal KnobInsetPx = 2.0;

QSize pillSize()
{
    const int h = theme::scaledPx(1.4, 14);
    return {h * 15 / 8, h};
}

} // namespace

SwitchBox::SwitchBox(const QString &text)
{
    setText(text);
    setCursor(Qt::PointingHandCursor);
}

QSize SwitchBox::sizeHint() const
{
    QSize s = pillSize();
    if (!text().isEmpty())
    {
        const QFontMetrics fm(font());
        s.rwidth() += theme::scaledPx(0.5) + fm.horizontalAdvance(text());
        s.rheight() = qMax(s.height(), fm.height());
    }
    return s;
}

QSize SwitchBox::minimumSizeHint() const
{
    return sizeHint();
}

bool SwitchBox::hitButton(const QPoint &pos) const
{
    return rect().contains(pos);
}

void SwitchBox::paintEvent(QPaintEvent * /*event*/)
{
    const bool on = isChecked();

    // Geometry first: the pill track, vertically centred in whatever height
    // the layout granted, and the knob at whichever end matches the state.
    const QSize pill = pillSize();
    const QRectF track(0, (height() - pill.height()) / 2.0, pill.width(), pill.height());
    const qreal diameter = track.height() - 2 * KnobInsetPx;
    const qreal knobX = on ? track.right() - KnobInsetPx - diameter : track.left() + KnobInsetPx;
    const QRectF knob(knobX, track.top() + KnobInsetPx, diameter, diameter);

    const AppPalette &pal = theme::current();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(on ? pal.primary : pal.input);
    painter.drawRoundedRect(track, track.height() / 2.0, track.height() / 2.0);
    painter.setBrush(on ? pal.primaryFg : pal.mutedFg);
    painter.drawEllipse(knob);

    if (!text().isEmpty())
    {
        painter.setPen(pal.foreground);
        const QRectF label(
            track.right() + theme::scaledPx(0.5), 0, width() - track.width(), height()
        );
        painter.drawText(label, Qt::AlignLeft | Qt::AlignVCenter, text());
    }
}
