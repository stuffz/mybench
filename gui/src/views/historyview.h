#pragma once
// The statement log the backend keeps per connection, searchable, with the
// two-click Clear that guards an irreversible wipe.
#include "views/panelbase.h"

class QPushButton;
class QTableWidget;
class QTimer;

class HistoryView final : public PanelBase
{
    Q_OBJECT
public:
    explicit HistoryView(const QString &connID, QWidget *parent = nullptr);

protected:
    void refresh() override;

private:
    QTableWidget *m_table;
    QLineEdit *m_search;
    QPushButton *m_clearBtn;
    bool m_armedClear = false;
    QTimer *m_armTimer;
};
