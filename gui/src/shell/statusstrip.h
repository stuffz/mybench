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

    // A transient confirmation or warning. The window's QStatusBar is hidden
    // for the life of the app, so this is the footer that users actually see.
    void showMessage(const QString &text, int ms);
    void applyTheme();

private:
    void tick();

    QString m_connID, m_label;
    QLabel *m_dot, *m_latency, *m_version, *m_uptime, *m_threads, *m_qps, *m_error;
    QLabel *m_message;
    // The dot and the latency read as one item, and share one tooltip that is
    // rebuilt per poll (health, round-trip, last check).
    QWidget *m_health;
    QTimer *m_timer;
    QTimer *m_messageTimer;
    double m_prevQuestions = -1;
    qint64 m_prevAt = 0;
};
