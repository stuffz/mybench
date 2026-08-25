#pragma once
// Global variables beside global status, both filtered by one box. The rows
// are kept in memory so filtering is a redraw rather than a round trip.
#include "views/panelbase.h"

class QTableWidget;

class ServerInfoView final : public PanelBase
{
    Q_OBJECT
public:
    explicit ServerInfoView(const QString &connID, QWidget *parent = nullptr);

protected:
    void refresh() override;

private:
    void fill();
    QTableWidget *m_vars;
    QTableWidget *m_status;
    QLineEdit *m_filter;
    QVector<QPair<QString, QString>> m_varRows, m_statusRows;
};
