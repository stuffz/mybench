#pragma once
// Footer for the active server: health dot, version, uptime, threads, QPS.
#include <QWidget>

class QLabel;
class QTimer;

class StatusStrip : public QWidget
{
    Q_OBJECT
public:
    explicit StatusStrip(QWidget *parent = nullptr);
    void watch(const QString &connID, const QString &label);
    void applyTheme();

private:
    void tick();

    QString m_connID, m_label;
    QLabel *m_dot, *m_latency, *m_version, *m_uptime, *m_threads, *m_qps, *m_error;
    // The dot and the latency read as one item, and share one tooltip that is
    // rebuilt per poll (health, round-trip, last check).
    QWidget *m_health;
    QTimer *m_timer;
    double m_prevQuestions = -1;
    qint64 m_prevAt = 0;
};
