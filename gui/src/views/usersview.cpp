#include "views/usersview.h"

#include "app/api.h"
#include "ui/tableutil.h"
#include "views/panelbase.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTableWidget>

UsersView::UsersView(const QString &connID, QWidget *parent)
    : PanelBase(tr("Users and Privileges"), connID, parent)
{
    m_table = makeTable({tr("User"), tr("Host"), tr("Plugin"), tr("Locked")});
    m_grants = new QPlainTextEdit;
    m_grants->setReadOnly(true);
    m_grants->setPlaceholderText(tr("Select an account to see its grants."));
    m_grants->setFrameShape(QFrame::NoFrame);

    auto *split = new QSplitter(Qt::Vertical);
    split->addWidget(m_table);
    split->addWidget(m_grants);
    split->setSizes({420, 220});
    setBody(split);

    connect(
        m_table, &QTableWidget::itemSelectionChanged, this,
        [this]()
        {
            const int row = m_table->currentRow();
            if (row < 0)
            {
                return;
            }
            loadGrants(m_table->item(row, 0)->text(), m_table->item(row, 1)->text());
        }
    );
    refresh();
}

void UsersView::refresh()
{
    api()->call(
        "admin", "Users", {m_connID}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                showError(err);
                return;
            }
            showError({});
            m_table->setSortingEnabled(false);
            m_table->setRowCount(0);
            for (const auto &v : res.toArray())
            {
                const QJsonObject o = v.toObject();
                const int row = m_table->rowCount();
                m_table->insertRow(row);
                m_table->setItem(row, 0, new QTableWidgetItem(o.value("user").toString()));
                m_table->setItem(row, 1, new QTableWidgetItem(o.value("host").toString()));
                m_table->setItem(row, 2, new QTableWidgetItem(o.value("plugin").toString()));
                m_table->setItem(
                    row, 3, new QTableWidgetItem(o.value("locked").toBool() ? tr("yes") : QString())
                );
            }
            m_table->setSortingEnabled(true);
            fitColumns(m_table);
        }
    );
}

void UsersView::loadGrants(const QString &user, const QString &host)
{
    api()->call(
        "admin", "Grants", {m_connID, user, host}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                m_grants->setPlainText(err);
                return;
            }
            QStringList lines;
            for (const auto &v : res.toArray())
            {
                lines << v.toString() + ";";
            }
            m_grants->setPlainText(lines.join('\n'));
        }
    );
}
