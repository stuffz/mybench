#pragma once
// One KPI tile: a glyph and label, the value, and a sub line for the context
// the value needs (a rate beside its total, a percentage beside its base).
#include <QColor>
#include <QString>
#include <QWidget>

class QLabel;

class StatCard : public QWidget
{
    // Q_OBJECT for the metaobject name alone: without it the stylesheet's
    // `StatCard { ... }` selector matches nothing and the tile loses its card.
    Q_OBJECT
public:
    StatCard(const QString &icon, const QString &label, QWidget *parent = nullptr);

    void setValue(const QString &value, const QString &sub = {});
    // Tones the value only — an alarming number should stand out without the
    // whole tile changing colour.
    void setTone(const QColor &tone);
    void applyTheme();

protected:
    // The sub line carries the context ("5 of 200 · peak 10") and is the
    // widest thing in the tile; eliding it here is what keeps twelve tiles
    // inside the window instead of forcing the page to scroll sideways.
    void resizeEvent(QResizeEvent *event) override;

private:
    void elide();

    QString m_iconName;
    QLabel *m_icon;
    QLabel *m_label;
    QLabel *m_value;
    QLabel *m_sub;
    QString m_valueText, m_subText;
    QColor m_tone;
};
