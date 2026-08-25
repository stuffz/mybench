#include "views/serverinfoview.h"

#include "app/api.h"
#include "ui/tableutil.h"
#include "views/panelbase.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QSplitter>
#include <QTableWidget>

ServerInfoView::ServerInfoView(const QString &connID, QWidget *parent)
    : PanelBase(tr("Server Info"), connID, parent)
{
    m_filter = addFilter(tr("Filter variables / status…"));
    connect(m_filter, &QLineEdit::textChanged, this, [this]() { fill(); });

    m_vars = makeTable({tr("Variable"), tr("Value")});
    m_status = makeTable({tr("Status"), tr("Value")});
    auto *split = new QSplitter(Qt::Horizontal);
    split->addWidget(m_vars);
    split->addWidget(m_status);
    setBody(split);
    refresh();
}

void ServerInfoView::refresh()
{
    api()->call(
        "admin", "ServerInfoSnapshot", {m_connID}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                showError(err);
                return;
            }
            showError({});
            auto take = [](const QJsonArray &arr)
            {
                QVector<QPair<QString, QString>> out;
                for (const auto &v : arr)
                {
                    const QJsonObject o = v.toObject();
                    out.append({o.value("name").toString(), o.value("value").toString()});
                }
                return out;
            };
            const QJsonObject o = res.toObject();
            m_varRows = take(o.value("variables").toArray());
            m_statusRows = take(o.value("status").toArray());
            fill();
        }
    );
}

void ServerInfoView::fill()
{
    const QString f = m_filter->text().trimmed();
    auto put = [&f](QTableWidget *t, const QVector<QPair<QString, QString>> &rows)
    {
        t->setSortingEnabled(false);
        t->setRowCount(0);
        for (const auto &kv : rows)
        {
            if (!f.isEmpty() && !kv.first.contains(f, Qt::CaseInsensitive) &&
                !kv.second.contains(f, Qt::CaseInsensitive))
            {
                continue;
            }
            const int row = t->rowCount();
            t->insertRow(row);
            t->setItem(row, 0, new QTableWidgetItem(kv.first));
            auto *val = new QTableWidgetItem(kv.second);
            val->setToolTip(kv.second);
            t->setItem(row, 1, val);
        }
        t->setSortingEnabled(true);
        fitColumns(t);
    };
    put(m_vars, m_varRows);
    put(m_status, m_statusRows);
}
