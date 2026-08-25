#include "views/historyview.h"

#include "app/api.h"
#include "app/theme.h"
#include "ui/tableutil.h"
#include "views/panelbase.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>

namespace
{

constexpr int HistoryLimit = 500;

} // namespace

HistoryView::HistoryView(const QString &connID, QWidget *parent)
    : PanelBase(tr("Query History"), connID, parent)
{
    m_search = addFilter(tr("Search Statements…"));
    connect(m_search, &QLineEdit::returnPressed, this, [this]() { refresh(); });

    m_clearBtn = new QPushButton(tr("Clear History"));
    // Destructive and irreversible: the first click arms, the second clears.
    m_armTimer = new QTimer(this);
    m_armTimer->setSingleShot(true);
    connect(
        m_armTimer, &QTimer::timeout, this,
        [this]()
        {
            m_armedClear = false;
            m_clearBtn->setText(tr("Clear History"));
            m_clearBtn->setProperty("variant", QVariant());
            m_clearBtn->style()->polish(m_clearBtn);
        }
    );
    connect(
        m_clearBtn, &QPushButton::clicked, this,
        [this]()
        {
            if (!m_armedClear)
            {
                m_armedClear = true;
                m_clearBtn->setText(tr("Confirm Clear"));
                m_clearBtn->setProperty("variant", "destructive");
                m_clearBtn->style()->polish(m_clearBtn);
                m_armTimer->start(4000);
                return;
            }
            m_armTimer->stop();
            m_armedClear = false;
            m_clearBtn->setText(tr("Clear History"));
            m_clearBtn->setProperty("variant", QVariant());
            m_clearBtn->style()->polish(m_clearBtn);
            api()->call(
                "query", "ClearHistory", {m_connID}, this,
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
    );
    addHeaderWidget(m_clearBtn);

    m_table = makeTable({tr("Started"), tr("Duration"), tr("Rows"), tr("Source"), tr("Statement")});
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
            m_table->selectRow(row);
            QMenu menu(this);
            menu.addAction(
                tr("Copy Statement"), this,
                [this, row]()
                {
                    // The raw statement — the cell shows a simplified() copy.
                    QApplication::clipboard()->setText(
                        m_table->item(row, 4)->data(Qt::UserRole).toString()
                    );
                }
            );
            menu.exec(m_table->viewport()->mapToGlobal(pos));
        }
    );
    setBody(m_table);
    refresh();
}

void HistoryView::refresh()
{
    api()->call(
        "query", "History", {m_connID, m_search->text(), HistoryLimit}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                showError(err);
                return;
            }
            showError({});
            const AppPalette &pal = theme::current();
            m_table->setSortingEnabled(false);
            m_table->setRowCount(0);
            for (const auto &v : res.toArray())
            {
                const QJsonObject o = v.toObject();
                const int row = m_table->rowCount();
                m_table->insertRow(row);
                // RFC3339 is too wide for the column; show the clock
                // time and keep the full stamp in the tooltip.
                const QString started = o.value("startedAt").toString();
                const QDateTime when = QDateTime::fromString(started, Qt::ISODate);
                auto *startedItem = new QTableWidgetItem(
                    when.isValid() ? when.toLocalTime().toString("yyyy-MM-dd HH:mm:ss") : started
                );
                startedItem->setToolTip(started);
                m_table->setItem(row, 0, startedItem);
                m_table->setItem(row, 1, numItem(qint64(o.value("durationMs").toDouble())));
                m_table->setItem(row, 2, numItem(qint64(o.value("rowCount").toDouble())));
                m_table->setItem(
                    row, 3,
                    new QTableWidgetItem(
                        o.value("source").toString().isEmpty() ? tr("editor")
                                                               : o.value("source").toString()
                    )
                );
                const QString q = o.value("query").toString();
                auto *stmt = new QTableWidgetItem(q.simplified());
                stmt->setToolTip(q);
                stmt->setData(Qt::UserRole, q); // Copy Statement copies this
                const QString e = o.value("error").toString();
                if (!e.isEmpty())
                {
                    stmt->setForeground(pal.destructive);
                    stmt->setToolTip(q + "\n\n" + e);
                }
                m_table->setItem(row, 4, stmt);
            }
            m_table->setSortingEnabled(true);
            fitColumns(m_table);
        }
    );
}
