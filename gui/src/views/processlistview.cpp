#include "views/processlistview.h"

#include "app/api.h"
#include "dialogs/querydialog.h"
#include "ui/tableutil.h"
#include "views/panelbase.h"

#include "ui/switchbox.h"
#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QScrollBar>
#include <QTableWidget>
#include <QTimer>

namespace
{

constexpr int PollChoicesSecs[] = {1, 2, 5, 10, 30};
constexpr int DefaultPollSecs = 2;
constexpr int MsPerSec = 1000;
constexpr int InfoColumn = 7;

} // namespace

ProcesslistView::ProcesslistView(const QString &connID, QWidget *parent)
    : PanelBase(tr("Client Connections"), connID, parent)
{
    m_hideSleeping = new SwitchBox(tr("Hide Sleeping"));
    m_hideSleeping->setChecked(true);
    connect(m_hideSleeping, &QCheckBox::toggled, this, [this]() { refresh(); });
    addHeaderStretch();
    addHeaderWidget(m_hideSleeping);

    m_autoRefresh = new SwitchBox(tr("Auto Refresh"));
    m_autoRefresh->setChecked(true);
    m_autoRefresh->setToolTip(tr("Poll the server on the chosen interval. Unchecked, the\n"
                                 "list only updates on Refresh."));
    connect(
        m_autoRefresh, &QCheckBox::toggled, this,
        [this](bool on)
        {
            m_interval->setEnabled(on);
            if (on)
            {
                refresh();
            }
        }
    );

    m_interval = new QComboBox;
    for (const int secs : PollChoicesSecs)
    {
        m_interval->addItem(tr("Every %1 s").arg(secs), secs * MsPerSec);
        if (secs == DefaultPollSecs)
        {
            m_interval->setCurrentIndex(m_interval->count() - 1);
        }
    }
    connect(
        m_interval, &QComboBox::currentIndexChanged, this,
        [this]() { m_timer->setInterval(m_interval->currentData().toInt()); }
    );

    auto *refreshRow = addHeaderRow();
    refreshRow->addWidget(m_autoRefresh);
    refreshRow->addWidget(m_interval);

    m_table = makeTable(
        {tr("Id"), tr("User"), tr("Host"), tr("DB"), tr("Command"), tr("Time"), tr("State"),
         tr("Info")}
    );
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
        m_table, &QWidget::customContextMenuRequested, this,
        [this](const QPoint &pos)
        {
            const int row = m_table->rowAt(pos.y());
            if (row < 0)
            {
                return;
            }
            const qint64 id = m_table->item(row, 0)->data(Qt::DisplayRole).toLongLong();
            const QString who = m_table->item(row, 1)->text() + "@" + m_table->item(row, 2)->text();
            QMenu menu(this);
            auto *show = menu.addAction(tr("Show Query"), this, [this, row]() { showQuery(row); });
            show->setEnabled(!m_table->item(row, InfoColumn)->text().isEmpty());
            menu.addSeparator();
            menu.addAction(tr("Kill Query"), this, [this, id, who]() { kill(id, true, who); });
            menu.addAction(
                tr("Kill Connection"), this, [this, id, who]() { kill(id, false, who); }
            );
            menu.exec(m_table->viewport()->mapToGlobal(pos));
        }
    );
    connect(
        m_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { showQuery(row); }
    );
    setBody(m_table);

    m_timer = new QTimer(this);
    m_timer->setInterval(m_interval->currentData().toInt());
    connect(
        m_timer, &QTimer::timeout, this,
        [this]()
        {
            if (m_autoRefresh->isChecked())
            {
                refresh();
            }
        }
    );
    m_timer->start();
    refresh();
}

void ProcesslistView::refresh()
{
    // Skip the tick while the previous request is in flight so a stalled
    // backend cannot pile up a queue of polls.
    if (m_inFlight)
    {
        return;
    }
    m_inFlight = true;
    api()->call(
        "admin", "Processlist", {m_connID}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            m_inFlight = false;
            if (!err.isEmpty())
            {
                showError(err);
                return;
            }
            showError({});
            const bool hide = m_hideSleeping->isChecked();
            // The rebuild would otherwise fight the user: remember the selected
            // thread and the scroll position, restore both afterwards.
            const int curRow = m_table->currentRow();
            const qint64 selId =
                curRow >= 0 ? m_table->item(curRow, 0)->data(Qt::DisplayRole).toLongLong() : -1;
            const int scrollAt = m_table->verticalScrollBar()->value();
            const bool sorting = m_table->isSortingEnabled();
            m_table->setSortingEnabled(false);
            m_table->setRowCount(0);
            for (const auto &v : res.toArray())
            {
                const QJsonObject o = v.toObject();
                if (hide &&
                    o.value("command").toString().compare("sleep", Qt::CaseInsensitive) == 0)
                {
                    continue;
                }
                const int row = m_table->rowCount();
                m_table->insertRow(row);
                m_table->setItem(row, 0, numItem(qint64(o.value("id").toDouble())));
                m_table->setItem(row, 1, new QTableWidgetItem(o.value("user").toString()));
                m_table->setItem(row, 2, new QTableWidgetItem(o.value("host").toString()));
                m_table->setItem(row, 3, new QTableWidgetItem(o.value("db").toString()));
                m_table->setItem(row, 4, new QTableWidgetItem(o.value("command").toString()));
                m_table->setItem(row, 5, numItem(qint64(o.value("time").toDouble())));
                m_table->setItem(row, 6, new QTableWidgetItem(o.value("state").toString()));
                auto *info = new QTableWidgetItem(o.value("info").toString());
                info->setToolTip(o.value("info").toString());
                m_table->setItem(row, InfoColumn, info);
            }
            m_table->setSortingEnabled(sorting);
            if (selId >= 0)
            {
                for (int r = 0; r < m_table->rowCount(); ++r)
                {
                    if (m_table->item(r, 0)->data(Qt::DisplayRole).toLongLong() == selId)
                    {
                        m_table->selectRow(r);
                        break;
                    }
                }
            }
            m_table->verticalScrollBar()->setValue(scrollAt);
            fitColumns(m_table);
        }
    );
}

void ProcesslistView::showQuery(int row)
{
    const QString sql = m_table->item(row, InfoColumn)->text();
    if (sql.isEmpty())
    {
        return;
    }
    const QString title = tr("Thread %1 (%2@%3)")
                              .arg(
                                  m_table->item(row, 0)->text(), m_table->item(row, 1)->text(),
                                  m_table->item(row, 2)->text()
                              );
    showQueryDialog(this, title, sql);
}

void ProcesslistView::kill(qint64 threadID, bool queryOnly, const QString &who)
{
    const QString title = queryOnly ? tr("Kill query?") : tr("Kill connection?");
    const QString body = queryOnly
                             ? tr("Stop the running statement on thread %1 (%2). The client stays "
                                  "connected.")
                                   .arg(threadID)
                                   .arg(who)
                             : tr("Disconnect thread %1 (%2). Any open transaction is rolled back.")
                                   .arg(threadID)
                                   .arg(who);
    if (QMessageBox::warning(this, title, body, QMessageBox::Cancel | QMessageBox::Yes) !=
        QMessageBox::Yes)
    {
        return;
    }
    api()->call(
        "admin", "Kill", {m_connID, threadID, queryOnly}, this,
        [this](const QJsonValue &, const QString &err)
        {
            if (!err.isEmpty())
            {
                showError(err);
            }
            refresh();
        }
    );
}
