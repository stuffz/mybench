#pragma once
// Shared chrome for the admin panels: a title, a Refresh button, an optional
// filter box — and an optional second toolbar row — above a body the subclass
// supplies. Keeps the panels from repeating twenty lines of layout each.
#include <QWidget>

class QHBoxLayout;
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
    void addHeaderStretch();
    QHBoxLayout *addHeaderRow();
    void setBody(QWidget *w);
    void showError(const QString &message);

    QString m_connID;
    QHBoxLayout *m_header;
    class QVBoxLayout *m_root;
    QLabel *m_error;

private:
    class QVBoxLayout *m_headerRows;
    QHBoxLayout *m_header2 = nullptr;
    bool m_headerStretched = false;
};
