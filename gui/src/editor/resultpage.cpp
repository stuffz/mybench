#include "editor/resultpage.h"

#include "app/api.h"
#include "app/theme.h"
#include "editor/resultgrid.h"
#include "editor/resultmodel.h"

#include <QJsonArray>
#include <QLabel>
#include <QStackedWidget>
#include <QVBoxLayout>

ResultPage::ResultPage(const QString &sql, QWidget *parent) : QWidget(parent), m_sql(sql)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_model = new ResultModel(this);
    m_grid = new ResultGrid;
    m_grid->setResultModel(m_model);

    m_summary = new QLabel;
    m_summary->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_summary->setContentsMargins(12, 10, 12, 10);
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_stack = new QStackedWidget;
    m_stack->addWidget(m_grid);
    m_stack->addWidget(m_summary);
    root->addWidget(m_stack);

    connect(m_grid, &ResultGrid::sortRequested, this, &ResultPage::sortRequested);
    connect(m_model, &ResultModel::stagedChanged, this, &ResultPage::stagedChanged);
    connect(m_model, &ResultModel::error, this, &ResultPage::errorRaised);
}

ResultPage::~ResultPage()
{
    closeResult();
}

void ResultPage::closeResult()
{
    if (m_resultId.isEmpty())
    {
        return;
    }
    api()->post("query", "CloseResult", {m_resultId});
    m_resultId.clear();
}

void ResultPage::applyState(const QJsonObject &state)
{
    const QString id = state.value("resultId").toString();
    const bool newResult = id != m_resultId;
    m_resultId = id;
    m_done = state.value("done").toBool();
    m_capped = state.value("capped").toBool();
    m_rowCount = state.value("rowCount").toInt();
    m_error = state.value("error").toString();

    QVector<ColumnMeta> cols;
    for (const auto &c : state.value("columns").toArray())
    {
        const QJsonObject o = c.toObject();
        cols.append({o.value("name").toString(), o.value("type").toString()});
    }
    // Run returns at column-metadata time, which for a streaming result means
    // the first state can still carry no columns — so adopt them whenever the
    // shape changes, not only on a new resultId.
    if (newResult || cols.size() != m_model->columns().size())
    {
        m_model->setResult(id, cols, m_rowCount);
        m_grid->resetFit();
    }
    else
    {
        m_model->setRowCount(m_rowCount);
    }
    m_grid->setSortable(m_done);

    const AppPalette &pal = theme::current();
    if (!m_error.isEmpty())
    {
        m_summary->setText(m_error);
        m_summary->setStyleSheet(QString("QLabel { color: %1; }").arg(pal.destructive.name()));
        m_stack->setCurrentWidget(m_summary);
    }
    else if (cols.isEmpty() && m_done)
    {
        // A statement with no result set still deserves a tab, so the position
        // of every other tab keeps matching the script.
        m_summary->setText(tr("Statement executed. No result set."));
        m_summary->setStyleSheet(QString("QLabel { color: %1; }").arg(pal.mutedFg.name()));
        m_stack->setCurrentWidget(m_summary);
    }
    else
    {
        m_stack->setCurrentWidget(m_grid);
    }

    emit stateChanged();
}

void ResultPage::resolveEditability()
{
    m_editable = false;
    m_insertSchema.clear();
    m_insertTable.clear();
    m_grid->clearInsertTarget();
    if (m_resultId.isEmpty() || !m_error.isEmpty())
    {
        return;
    }
    const QString id = m_resultId;
    api()->call(
        "query", "EditInfo", {id}, this,
        [this, id](const QJsonValue &res, const QString &err)
        {
            if (id != m_resultId || !err.isEmpty())
            {
                return; // not editable is the safe default
            }
            const QJsonObject o = res.toObject();
            if (!o.value("editable").toBool())
            {
                return;
            }
            QHash<QString, int> byName;
            for (int i = 0; i < m_model->columns().size(); ++i)
            {
                byName.insert(m_model->columns().at(i).name, i);
            }

            QSet<int> editable;
            for (const auto &v : o.value("editableCols").toArray())
            {
                const int ix = byName.value(v.toString(), -1);
                if (ix >= 0)
                {
                    editable.insert(ix);
                }
            }
            QVector<QPair<QString, int>> keys;
            for (const auto &v : o.value("keyCols").toArray())
            {
                const int ix = byName.value(v.toString(), -1);
                if (ix >= 0)
                {
                    keys.append({v.toString(), ix});
                }
            }
            m_model->setEditable(true, editable, keys);
            m_editable = true;
            m_insertSchema = o.value("schema").toString();
            m_insertTable = o.value("table").toString();
            if (!m_insertTable.isEmpty())
            {
                m_grid->setInsertTarget(m_insertSchema, m_insertTable);
            }
            emit stateChanged();
        }
    );
}
