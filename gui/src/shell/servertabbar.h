#pragma once
// One tab per open connection: accent bar, name, close. Square tabs with the
// connection colour across the top, like the web ServerTabs row.
#include <QColor>
#include <QHash>
#include <QVector>
#include <QWidget>

class QHBoxLayout;

class ServerTabBar : public QWidget
{
    Q_OBJECT
public:
    explicit ServerTabBar(QWidget *parent = nullptr);
    void setConnections(
        const QVector<QPair<QString, QString>> &idsAndNames, const QHash<QString, QColor> &colors,
        const QString &active
    );
    void applyTheme();

signals:
    void activated(const QString &connID);
    void closeRequested(const QString &connID);

private:
    QVector<QPair<QString, QString>> m_conns;
    QHash<QString, QColor> m_colors;
    QString m_active;

protected:
    // The whole tab is clickable, not just a button inside it.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QHBoxLayout *m_row;
};
