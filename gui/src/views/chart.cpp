#include "views/chart.h"

#include "app/theme.h"

#include <QFontMetrics>
#include <QLocale>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace
{

constexpr int PadLeft = 56; // room for the y-axis labels
constexpr int PadRight = 10;
constexpr int PadTop = 48; // title and subtitle rows above the plot
constexpr int PadBottom = 10;
constexpr int Radius = 8;

// Counters on a busy server run to millions; spelled out in full they push the
// y labels out of their gutter and the legend over the title.
QString compact(double v)
{
    const QLocale loc;
    const double a = std::fabs(v);
    if (a >= 1e9)
    {
        return loc.toString(v / 1e9, 'f', a < 1e10 ? 1 : 0) + "G";
    }
    if (a >= 1e6)
    {
        return loc.toString(v / 1e6, 'f', a < 1e7 ? 1 : 0) + "M";
    }
    if (a >= 10000)
    {
        return loc.toString(v / 1e3, 'f', 0) + "k";
    }
    if (a > 0 && a < 10)
    {
        return loc.toString(v, 'f', 1);
    }
    return loc.toString(qint64(std::llround(v)));
}

// "Nice" axis maximum: the smallest 1/2/5 × 10ⁿ above the peak, so the top
// label reads 200 or 500 rather than 187.3.
double niceCeiling(double peak)
{
    if (peak <= 0)
    {
        return 1;
    }
    const double mag = std::pow(10.0, std::floor(std::log10(peak)));
    const double norm = peak / mag;
    double step = 10;
    if (norm <= 1)
    {
        step = 1;
    }
    else if (norm <= 2)
    {
        step = 2;
    }
    else if (norm <= 5)
    {
        step = 5;
    }
    return step * mag;
}

} // namespace

TimeChart::TimeChart(const QString &title, QWidget *parent) : QWidget(parent), m_title(title)
{
    setMinimumHeight(150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setMouseTracking(true);
}

void TimeChart::addSeries(const QString &name, const QColor &colour)
{
    m_series.append({name, colour, {}});
}

void TimeChart::push(const QVector<double> &values)
{
    for (int i = 0; i < m_series.size(); ++i)
    {
        Series &s = m_series[i];
        s.pts.append(i < values.size() ? values.at(i) : 0.0);
        if (s.pts.size() > TimeChart::Capacity)
        {
            s.pts.removeFirst();
        }
    }
    update();
}

void TimeChart::setFormat(Format f)
{
    m_format = f;
}

void TimeChart::setCeiling(double ceiling)
{
    m_ceiling = ceiling;
}

void TimeChart::setSubtitle(const QString &text)
{
    m_subtitle = text;
    update();
}

void TimeChart::setSeriesColours(const QVector<QColor> &colours)
{
    for (int i = 0; i < m_series.size() && i < colours.size(); ++i)
    {
        m_series[i].colour = colours.at(i);
    }
    update();
}

void TimeChart::clearSamples()
{
    for (Series &s : m_series)
    {
        s.pts.clear();
    }
    m_hover = -1;
    update();
}

QSize TimeChart::sizeHint() const
{
    return {320, 168};
}

int TimeChart::sampleCount() const
{
    int n = 0;
    for (const Series &s : m_series)
    {
        n = std::max(n, int(s.pts.size()));
    }
    return n;
}

QRect TimeChart::plotRect() const
{
    return QRect(
        PadLeft, PadTop, std::max(1, width() - PadLeft - PadRight),
        std::max(1, height() - PadTop - PadBottom)
    );
}

QString TimeChart::format(double v) const
{
    const QLocale loc;
    switch (m_format)
    {
    case Format::Percent:
        return loc.toString(v, 'f', v < 10 ? 1 : 0) + "%";
    case Format::Bytes:
    {
        static const char *unit[] = {"B", "KB", "MB", "GB", "TB"};
        int u = 0;
        double n = v;
        while (n >= 1024 && u < 4)
        {
            n /= 1024;
            ++u;
        }
        return loc.toString(n, 'f', n < 10 && u > 0 ? 1 : 0) + " " + unit[u] + "/s";
    }
    case Format::Number:
        break;
    }
    return compact(v);
}

void TimeChart::paintEvent(QPaintEvent *)
{
    const AppPalette &pal = theme::current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Card
    p.setPen(QPen(pal.border, 1));
    p.setBrush(pal.card);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), Radius, Radius);

    // Painter text: the stylesheet cannot reach it, so it derives from the
    // slider in px. Points would drift with the screen DPI, and arithmetic on
    // pointSizeF() would read -1 from the pixel-sized base font.
    QFont small = font();
    small.setPixelSize(theme::scaledPx(0.85));

    // Title, and the sample the legend is describing.
    QFont titleFont = font();
    titleFont.setWeight(QFont::DemiBold);
    p.setFont(titleFont);
    p.setPen(pal.foreground);
    p.drawText(QRect(12, 8, width() - 24, 18), Qt::AlignLeft | Qt::AlignVCenter, m_title);
    const int titleRight = 12 + QFontMetrics(titleFont).horizontalAdvance(m_title);

    const int count = sampleCount();
    const int at = (m_hover >= 0 && m_hover < count) ? m_hover : count - 1;

    p.setFont(small);
    if (!m_subtitle.isEmpty())
    {
        p.setPen(pal.mutedFg);
        p.drawText(
            QRect(12, 24, width() - 24, 14), Qt::AlignLeft | Qt::AlignVCenter,
            m_hover >= 0 ? tr("%1 · %2 samples back").arg(m_subtitle).arg(count - 1 - m_hover)
                         : m_subtitle
        );
    }

    // Legend, right-aligned, each entry showing the value at `at`.
    int x = width() - 12;
    for (qsizetype i = m_series.size() - 1; i >= 0; --i)
    {
        const Series &s = m_series.at(i);
        const double v = (at >= 0 && at < s.pts.size()) ? s.pts.at(at) : 0.0;
        const QString text = s.name + "  " + format(v);
        const int w = QFontMetrics(small).horizontalAdvance(text);
        p.setPen(s.colour);
        p.drawText(QRect(x - w, 8, w, 16), Qt::AlignRight | Qt::AlignVCenter, text);
        p.setBrush(s.colour);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(x - w - 12, 14, 8, 3), 1.5, 1.5);
        x -= w + 22;
        if (x - 12 < titleRight + 16)
        {
            break; // out of room: the remaining series keep their line, not their label
        }
    }

    const QRect plot = plotRect();

    // Y scale: fixed for percentages, otherwise the window's peak.
    double top = m_ceiling;
    if (top <= 0)
    {
        double peak = 0;
        for (const Series &s : m_series)
        {
            for (const double v : s.pts)
            {
                peak = std::max(peak, v);
            }
        }
        top = niceCeiling(peak * 1.05);
    }

    // Gridlines and their labels.
    QColor grid = pal.border;
    grid.setAlpha(140);
    p.setPen(QPen(grid, 1, Qt::DotLine));
    QColor labelColour = pal.mutedFg;
    for (int i = 0; i <= 2; ++i)
    {
        const double frac = i / 2.0;
        const int y = plot.bottom() - int(frac * plot.height());
        p.setPen(QPen(grid, 1, i == 0 ? Qt::SolidLine : Qt::DotLine));
        p.drawLine(plot.left(), y, plot.right(), y);
        p.setPen(labelColour);
        p.drawText(
            QRect(6, y - 8, PadLeft - 12, 16), Qt::AlignRight | Qt::AlignVCenter, format(top * frac)
        );
    }

    if (count < 2)
    {
        p.setPen(labelColour);
        p.drawText(plot, Qt::AlignCenter, tr("collecting…"));
        return;
    }

    // Newest sample at the right edge, history trailing off to the left: a
    // fresh tab draws at the right and grows leftwards rather than pinning a
    // short line to the left of an empty card.
    const double dx = double(plot.width()) / (TimeChart::Capacity - 1);
    auto pointAt = [&](const Series &s, int i)
    {
        const double v = std::clamp(s.pts.at(i) / top, 0.0, 1.0);
        return QPointF(
            plot.right() - double(s.pts.size() - 1 - i) * dx, plot.bottom() - v * plot.height()
        );
    };

    for (const Series &s : m_series)
    {
        if (s.pts.size() < 2)
        {
            continue;
        }
        QPainterPath line;
        line.moveTo(pointAt(s, 0));
        for (int i = 1; i < s.pts.size(); ++i)
        {
            line.lineTo(pointAt(s, i));
        }

        // A single series reads better filled; several would muddy each other.
        if (m_series.size() == 1)
        {
            QPainterPath area = line;
            area.lineTo(pointAt(s, int(s.pts.size()) - 1).x(), plot.bottom());
            area.lineTo(pointAt(s, 0).x(), plot.bottom());
            area.closeSubpath();
            QColor fill = s.colour;
            fill.setAlpha(38);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawPath(area);
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(s.colour, 1.6));
        p.drawPath(line);
    }

    if (m_hover >= 0 && m_hover < count)
    {
        const double hx = plot.right() - (count - 1 - m_hover) * dx;
        p.setPen(QPen(pal.mutedFg, 1, Qt::DashLine));
        p.drawLine(QPointF(hx, plot.top()), QPointF(hx, plot.bottom()));
        for (const Series &s : m_series)
        {
            if (m_hover >= s.pts.size())
            {
                continue;
            }
            p.setPen(Qt::NoPen);
            p.setBrush(s.colour);
            p.drawEllipse(pointAt(s, m_hover), 2.5, 2.5);
        }
    }
}

void TimeChart::mouseMoveEvent(QMouseEvent *event)
{
    const QRect plot = plotRect();
    const int count = sampleCount();
    if (count < 2 || !plot.contains(event->pos()))
    {
        if (m_hover != -1)
        {
            m_hover = -1;
            update();
        }
        return;
    }
    const double dx = double(plot.width()) / (TimeChart::Capacity - 1);
    const int i = std::clamp(
        count - 1 - int(std::lround((plot.right() - event->position().x()) / dx)), 0, count - 1
    );
    if (i != m_hover)
    {
        m_hover = i;
        update();
    }
}

void TimeChart::leaveEvent(QEvent *)
{
    if (m_hover != -1)
    {
        m_hover = -1;
        update();
    }
}
