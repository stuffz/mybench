#include "views/inspectorview.h"

#include "app/api.h"
#include "ui/fmt.h"
#include "ui/tableutil.h"

#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace
{

// A section's id (what loadSection dispatches on) and its tab label.
using Section = QPair<QString, QString>;

} // namespace

InspectorView::InspectorView(
    const QString &connID, const QString &schema, const QString &table,
    const QString &initialSection, QWidget *parent
)
    : QWidget(parent), m_connID(connID), m_schema(schema), m_table(table)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_tabs = new QTabWidget;
    // No document mode — it suppresses the stylesheet pane frame that draws
    // the separator under the tab strip (see theme.cpp).
    root->addWidget(m_tabs, 1);

    // --- the sections this target has -------------------------------------
    // Info is common ground; a table then gets the structure tabs, a schema
    // gets the list of what it holds.
    QVector<Section> sections{{"info", tr("Info")}};
    if (!m_table.isEmpty())
    {
        sections << Section{"columns", tr("Columns")} << Section{"indexes", tr("Indexes")}
                 << Section{"fks", tr("Foreign Keys")} << Section{"ddl", tr("DDL")};
    }
    else
    {
        sections << Section{"tables", tr("Tables")};
    }

    // --- one page per section ---------------------------------------------
    // Every section but the DDL is a table. Nothing is filled in here: the
    // pages load on first visit, so opening the inspector costs one query.
    for (const Section &section : sections)
    {
        QWidget *page = nullptr;
        if (section.first == "info")
        {
            page = makeTable({tr("Property"), tr("Value")});
        }
        else if (section.first == "columns")
        {
            page = makeTable(
                {tr("Column"), tr("Type"), tr("Key"), tr("Null"), tr("Default"), tr("Extra"),
                 tr("Comment")}
            );
        }
        else if (section.first == "indexes")
        {
            page =
                makeTable({tr("Index"), tr("Columns"), tr("Unique"), tr("Type"), tr("Cardinality")}
                );
        }
        else if (section.first == "fks")
        {
            page = makeTable(
                {tr("Name"), tr("Columns"), tr("References"), tr("On Update"), tr("On Delete")}
            );
        }
        else if (section.first == "tables")
        {
            page = makeTable(
                {tr("Table"), tr("Type"), tr("Engine"), tr("Rows"), tr("Data"), tr("Index"),
                 tr("Collation")}
            );
        }
        else if (section.first == "ddl")
        {
            auto *ddl = new QPlainTextEdit;
            ddl->setReadOnly(true);
            ddl->setLineWrapMode(QPlainTextEdit::NoWrap);
            ddl->setFrameShape(QFrame::NoFrame);
            page = ddl;
        }

        m_pages.insert(section.first, page);
        const int ix = m_tabs->addTab(page, section.second);
        m_indexById.insert(ix, section.first);
    }

    connect(
        m_tabs, &QTabWidget::currentChanged, this,
        [this](int ix) { loadSection(m_indexById.value(ix)); }
    );

    // --- the section to open on --------------------------------------------
    // Restored from the workspace, so a reopened inspector lands where it was
    // left; the currentChanged wiring above is already live, but a tab that is
    // already current emits nothing, hence the explicit load.
    const int want = m_tabs->indexOf(
        m_pages.value(initialSection.isEmpty() ? QStringLiteral("info") : initialSection)
    );
    m_tabs->setCurrentIndex(want >= 0 ? want : 0);
    loadSection(m_indexById.value(m_tabs->currentIndex()));
}

void InspectorView::loadSection(const QString &id)
{
    if (id.isEmpty() || m_loaded.value(id))
    {
        return;
    }
    m_loaded.insert(id, true);
    if (id == "info")
    {
        loadInfo();
    }
    else if (id == "columns")
    {
        loadColumns();
    }
    else if (id == "indexes")
    {
        loadIndexes();
    }
    else if (id == "fks")
    {
        loadForeignKeys();
    }
    else if (id == "ddl")
    {
        loadDdl();
    }
    else if (id == "tables")
    {
        loadTables();
    }
}

void InspectorView::failSection(const QString &id, QTableWidget *t, const QString &err)
{
    m_loaded.insert(id, false);
    putRow(t, {tr("error: %1").arg(err)});
    fitColumns(t);
}

void InspectorView::loadInfo()
{
    auto *t = qobject_cast<QTableWidget *>(m_pages.value("info"));
    if (!t)
    {
        return;
    }
    if (m_table.isEmpty())
    {
        api()->call(
            "admin", "SchemaInfo", {m_connID, m_schema}, this,
            [this, t](const QJsonValue &res, const QString &err)
            {
                t->setRowCount(0);
                if (!err.isEmpty())
                {
                    failSection("info", t, err);
                    return;
                }
                const QJsonObject o = res.toObject();
                const SortPause pause(t);
                putRow(t, {tr("Schema"), m_schema});
                putRow(t, {tr("Charset"), o.value("charset").toString()});
                putRow(t, {tr("Collation"), o.value("collation").toString()});
                fitColumns(t);
            }
        );
        return;
    }
    api()->call(
        "admin", "TablesInfo", {m_connID, m_schema, m_table}, this,
        [this, t](const QJsonValue &res, const QString &err)
        {
            t->setRowCount(0);
            if (!err.isEmpty())
            {
                failSection("info", t, err);
                return;
            }
            const QJsonArray arr = res.toArray();
            if (arr.isEmpty())
            {
                return;
            }
            const QJsonObject o = arr.first().toObject();
            const SortPause pause(t);
            putRow(t, {tr("Table"), m_schema + "." + o.value("name").toString()});
            putRow(t, {tr("Type"), o.value("type").toString()});
            putRow(t, {tr("Engine"), o.value("engine").toString()});
            putRow(t, {tr("Row Format"), o.value("rowFormat").toString()});
            putRow(
                t, {tr("Rows (estimate)"), QString("%L1").arg(qint64(o.value("rows").toDouble()))}
            );
            putRow(t, {tr("Avg Row Length"), fmtBytes(qint64(o.value("avgRowLength").toDouble()))});
            putRow(t, {tr("Data Length"), fmtBytes(qint64(o.value("dataLength").toDouble()))});
            putRow(t, {tr("Index Length"), fmtBytes(qint64(o.value("indexLength").toDouble()))});
            putRow(
                t,
                {tr("Auto Increment"), QString::number(qint64(o.value("autoIncrement").toDouble()))}
            );
            putRow(t, {tr("Collation"), o.value("collation").toString()});
            putRow(t, {tr("Created"), o.value("created").toString()});
            putRow(t, {tr("Updated"), o.value("updated").toString()});
            fitColumns(t);
        }
    );
}

void InspectorView::loadColumns()
{
    auto *t = qobject_cast<QTableWidget *>(m_pages.value("columns"));
    if (!t)
    {
        return;
    }
    api()->call(
        "admin", "Columns", {m_connID, m_schema, m_table}, this,
        [this, t](const QJsonValue &res, const QString &err)
        {
            t->setRowCount(0);
            if (!err.isEmpty())
            {
                failSection("columns", t, err);
                return;
            }
            const SortPause pause(t);
            for (const auto &v : res.toArray())
            {
                const QJsonObject o = v.toObject();
                putRow(
                    t, {o.value("name").toString(), o.value("type").toString(),
                        o.value("key").toString(), o.value("nullable").toBool() ? "YES" : "NO",
                        o.value("default").toString(), o.value("extra").toString(),
                        o.value("comment").toString()}
                );
            }
            fitColumns(t);
        }
    );
}

void InspectorView::loadIndexes()
{
    auto *t = qobject_cast<QTableWidget *>(m_pages.value("indexes"));
    if (!t)
    {
        return;
    }
    api()->call(
        "admin", "Indexes", {m_connID, m_schema, m_table}, this,
        [this, t](const QJsonValue &res, const QString &err)
        {
            t->setRowCount(0);
            if (!err.isEmpty())
            {
                failSection("indexes", t, err);
                return;
            }
            const SortPause pause(t);
            for (const auto &v : res.toArray())
            {
                const QJsonObject o = v.toObject();
                const auto card = qint64(o.value("cardinality").toDouble());
                putRow(
                    t, {new QTableWidgetItem(o.value("name").toString()),
                        new QTableWidgetItem(o.value("columns").toString()),
                        new QTableWidgetItem(o.value("unique").toBool() ? "YES" : ""),
                        new QTableWidgetItem(o.value("type").toString()),
                        numItem(card, QString("%L1").arg(card))}
                );
            }
            fitColumns(t);
        }
    );
}

void InspectorView::loadForeignKeys()
{
    auto *t = qobject_cast<QTableWidget *>(m_pages.value("fks"));
    if (!t)
    {
        return;
    }
    api()->call(
        "admin", "ForeignKeys", {m_connID, m_schema, m_table}, this,
        [this, t](const QJsonValue &res, const QString &err)
        {
            t->setRowCount(0);
            if (!err.isEmpty())
            {
                failSection("fks", t, err);
                return;
            }
            const SortPause pause(t);
            for (const auto &v : res.toArray())
            {
                const QJsonObject o = v.toObject();
                putRow(
                    t, {o.value("name").toString(), o.value("columns").toString(),
                        o.value("refSchema").toString() + "." + o.value("refTable").toString() +
                            " (" + o.value("refColumns").toString() + ")",
                        o.value("onUpdate").toString(), o.value("onDelete").toString()}
                );
            }
            fitColumns(t);
        }
    );
}

void InspectorView::loadDdl()
{
    auto *e = qobject_cast<QPlainTextEdit *>(m_pages.value("ddl"));
    if (!e)
    {
        return;
    }
    api()->call(
        "admin", "ShowCreate", {m_connID, m_schema, m_table}, this,
        [this, e](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                m_loaded.insert("ddl", false); // retry on the next visit
            }
            e->setPlainText(err.isEmpty() ? res.toString() : err);
        }
    );
}

void InspectorView::loadTables()
{
    auto *t = qobject_cast<QTableWidget *>(m_pages.value("tables"));
    if (!t)
    {
        return;
    }
    api()->call(
        "admin", "TablesInfo", {m_connID, m_schema, QString()}, this,
        [this, t](const QJsonValue &res, const QString &err)
        {
            t->setRowCount(0);
            if (!err.isEmpty())
            {
                failSection("tables", t, err);
                return;
            }
            const SortPause pause(t);
            for (const auto &v : res.toArray())
            {
                const QJsonObject o = v.toObject();
                const auto rows = qint64(o.value("rows").toDouble());
                const auto data = qint64(o.value("dataLength").toDouble());
                const auto index = qint64(o.value("indexLength").toDouble());
                putRow(
                    t, {new QTableWidgetItem(o.value("name").toString()),
                        new QTableWidgetItem(o.value("type").toString()),
                        new QTableWidgetItem(o.value("engine").toString()),
                        numItem(rows, QString("%L1").arg(rows)), numItem(data, fmtBytes(data)),
                        numItem(index, fmtBytes(index)),
                        new QTableWidgetItem(o.value("collation").toString())}
                );
            }
            fitColumns(t);
        }
    );
}
