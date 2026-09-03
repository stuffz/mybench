#pragma once
// The client connection list: SHOW PROCESSLIST on a short poll, with the two
// guarded KILL actions on the context menu.
#include "views/panelbase.h"

class QCheckBox;
class QComboBox;
class QTableWidget;
class QTimer;

class ProcesslistView final : public PanelBase
{
    Q_OBJECT
public:
    explicit ProcesslistView(const QString &connID, QWidget *parent = nullptr);

protected:
    void refresh() override;

private:
    void showQuery(int row);
    void kill(qint64 threadID, bool queryOnly, const QString &who);

    QTableWidget *m_table;
    QCheckBox *m_hideSleeping;
    QCheckBox *m_autoRefresh;
    QComboBox *m_interval;
    QTimer *m_timer;
    bool m_inFlight = false; // the poll skips a tick while a request is out
};
