#pragma once
// The server dashboard: the counters worth watching, the rates derived from
// them, and InnoDB's engine detail on one scrolling page.
//
// The backend samples (admin.Dashboard); the rate maths and the sample history
// live here, because the history is per-open-tab — closing the tab is what
// forgets it, and two tabs on the same server keep their own windows.
#include "views/panelbase.h"

#include <QHash>
#include <QJsonObject>
#include <QVector>

class QCheckBox;
class QGridLayout;
class QLabel;
class QPlainTextEdit;
class QTableWidget;
class QTimer;
class StatCard;
class TimeChart;

class DashboardView final : public PanelBase
{
    Q_OBJECT
public:
    explicit DashboardView(const QString &connID, QWidget *parent = nullptr);

protected:
    void refresh() override;

private:
    void buildCards(QGridLayout *grid);
    void buildCharts(QGridLayout *grid);
    void applySnapshot(const QJsonObject &snap);
    void fillEngine();
    void fillConfig();
    void fillTrx(const QJsonObject &snap);
    void fillLockWaits(const QJsonObject &snap);
    void refreshSections();
    void applyTheme();

    // Counter readers over the current snapshot.
    qint64 st(const QString &key) const;
    qint64 im(const QString &key) const;
    // Per-second rate of a status counter between the last two samples; 0
    // before the second sample, and 0 across a counter reset (a restart).
    double rate(const QString &key) const;
    double poolHitRate() const;

    QCheckBox *m_live;
    QLabel *m_meta;
    QTimer *m_timer;
    QTimer *m_sectionTimer;

    QHash<QString, StatCard *> m_cards;
    QHash<QString, TimeChart *> m_charts;
    QTableWidget *m_engine;
    QTableWidget *m_config;
    QTableWidget *m_trx;
    QTableWidget *m_lockWaits;
    QLabel *m_trxLabel;
    QLabel *m_lockLabel;
    QPlainTextEdit *m_deadlock;
    QPlainTextEdit *m_fkError;
    QPlainTextEdit *m_semaphores;

    QJsonObject m_status, m_innodb, m_vars;
    QJsonObject m_prevStatus;
    qint64 m_prevAt = 0;
    double m_dt = 0;
    // The polls skip a tick while their previous request is still in flight.
    bool m_inFlight = false;
    bool m_sectionsInFlight = false;
};
