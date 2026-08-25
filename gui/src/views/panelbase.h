#pragma once
// Shared chrome for the admin panels: a title, a Refresh button and an
// optional filter box above a body the subclass supplies. Keeps the panels
// from repeating twenty lines of layout each.
#include <QWidget>

class QLabel;
class QLineEdit;

class PanelBase : public QWidget
{
    Q_OBJECT
public:
    PanelBase(const QString &title, const QString &connID, QWidget *parent = nullptr);

protected:
    virtual void refresh() = 0;
    QLineEdit *addFilter(const QString &placeholder);
    void addHeaderWidget(QWidget *w);
    void setBody(QWidget *w);
    void showError(const QString &message);

    QString m_connID;
    class QHBoxLayout *m_header;
    class QVBoxLayout *m_root;
    QLabel *m_error;
};
