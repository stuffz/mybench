#pragma once
// A small time-series chart: one card, a few series, a fixed-length window of
// samples. Painted directly because that is all this needs — no chart library,
// no scene graph, and it reads theme::current() per paint so a theme switch
// costs nothing.
#include <QColor>
#include <QString>
#include <QVector>
#include <QWidget>

class TimeChart : public QWidget
{
    Q_OBJECT
public:
    // Samples kept per series. Six minutes at the dashboard's two-second
    // poll: long enough to watch a spike arrive and drain, short enough that
    // every sample still gets a pixel column inside a card.
    static constexpr int Capacity = 180;

    // How series values are written in the legend and the y axis.
    enum class Format
    {
        Number,
        Bytes,
        Percent
    };

    explicit TimeChart(const QString &title, QWidget *parent = nullptr);

    void addSeries(const QString &name, const QColor &colour);
    // One value per series, in the order they were added. The oldest sample
    // falls off the left once the window is full.
    void push(const QVector<double> &values);
    void setFormat(Format f);
    // Fixed y maximum — percentages want 100 whether or not anything reached
    // it. Zero (the default) auto-scales to the window's peak.
    void setCeiling(double ceiling);
    void setSubtitle(const QString &text);
    // Re-colour in place on a theme switch; sample history is kept.
    void setSeriesColours(const QVector<QColor> &colours);
    void clearSamples();

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    // Hovering reads the sample under the cursor: the legend switches from
    // "now" to that sample and a guide marks it. Cheaper and steadier to read
    // than a tooltip that follows the mouse.
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct Series
    {
        QString name;
        QColor colour;
        QVector<double> pts;
    };

    QString format(double v) const;
    int sampleCount() const;
    QRect plotRect() const;

    QString m_title, m_subtitle;
    QVector<Series> m_series;
    Format m_format = Format::Number;
    double m_ceiling = 0;
    int m_hover = -1;
};
