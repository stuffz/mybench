#pragma once
// Accounts and privileges: the account list above the grants of whichever row
// is selected.
#include "views/panelbase.h"

class QPlainTextEdit;
class QTableWidget;

class UsersView final : public PanelBase
{
    Q_OBJECT
public:
    explicit UsersView(const QString &connID, QWidget *parent = nullptr);

protected:
    void refresh() override;

private:
    void loadGrants(const QString &user, const QString &host);
    QTableWidget *m_table;
    QPlainTextEdit *m_grants;
};
