#include "views/inspectorview.h"

#include "app/api.h"
#include "app/stubbackend.h"
#include "app/theme.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLatin1String>
#include <QLocale>
#include <QObject>
#include <QPlainTextEdit>
#include <QString>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTest>
#include <QVariant>
#include <QWidget>

// The inspector is five (a table) or two (a schema) tabs over one connection,
// each tab one admin call that fills one page. Nothing is fetched until its tab
// is visited, so what a test can reach is: which call went out, what the cells
// then read, and what is left standing when the call comes back an error.
//
// Rows are looked up by their first cell, never by index: makeTable leaves
// sorting live, so SortPause re-sorts every section the moment the loader's
// scope ends and the insertion order is not what the table keeps.
namespace
{

constexpr auto ConnID = "conn-1";
constexpr auto Schema = "shop";
constexpr auto Table = "orders";
constexpr int FontSize = 13;
constexpr int FlushMs = 5000;

constexpr auto TablesInfoPath = "/rpc/admin/TablesInfo";
constexpr auto SchemaInfoPath = "/rpc/admin/SchemaInfo";
constexpr auto ColumnsPath = "/rpc/admin/Columns";
constexpr auto IndexesPath = "/rpc/admin/Indexes";
constexpr auto ForeignKeysPath = "/rpc/admin/ForeignKeys";
constexpr auto ShowCreatePath = "/rpc/admin/ShowCreate";

// The section ids the constructor restores a workspace with, and the tab
// labels they carry. Both are part of the contract: the id comes back off disk
// and the label is what the user clicks.
constexpr auto InfoSection = "info";
constexpr auto ColumnsSection = "columns";
constexpr auto IndexesSection = "indexes";
constexpr auto FksSection = "fks";
constexpr auto DdlSection = "ddl";
constexpr auto TablesSection = "tables";

constexpr auto InfoTab = "Info";
constexpr auto ColumnsTab = "Columns";
constexpr auto IndexesTab = "Indexes";
constexpr auto ForeignKeysTab = "Foreign Keys";
constexpr auto DdlTab = "DDL";
constexpr auto TablesTab = "Tables";

// The Info page for a table, one row per property and the comment dropped.
constexpr int InfoRows = 12;

// Values paired with what the page has to print them as: the counts go through
// "%L1" and the lengths through fmtBytes, and the two spell a number
// differently.
constexpr int RowEstimate = 1234567; // "1,234,567"
constexpr int AvgRowBytes = 1536;    // "1.5 KiB"
constexpr int DataBytes = 1048576;   // "1.0 MiB"
constexpr int IndexBytes = 524288;   // "512 KiB"
constexpr int AutoIncrement = 90210; // "90210", ungrouped
constexpr int IndexCardinality = 4096;

constexpr auto Collation = "utf8mb4_0900_ai_ci";

constexpr auto Ddl = "CREATE TABLE `orders` (\n"
                     "  `id` bigint unsigned NOT NULL AUTO_INCREMENT,\n"
                     "  PRIMARY KEY (`id`)\n"
                     ") ENGINE=InnoDB";

constexpr auto ColumnsDenied = "admin.Columns: SELECT command denied to user 'ro'@'%'";
constexpr auto DdlDenied = "admin.ShowCreate: SHOW command denied to user 'ro'@'%'";

// The positional argument lists internal/rpc unpacks on the other end.
QJsonArray tableArgs()
{
    return QJsonArray{
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table)
    };
}

QJsonArray schemaArgs()
{
    return QJsonArray{QString::fromLatin1(ConnID), QString::fromLatin1(Schema)};
}

// TablesInfo narrows to one table with its third argument, so the whole-schema
// listing has to send it empty rather than leave it off.
QJsonArray wholeSchemaArgs()
{
    return QJsonArray{QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString()};
}

// One information_schema.tables row (backend/internal/admin/inspect.go).
QJsonObject tableInfo()
{
    return QJsonObject{
        {"name", Table},
        {"type", "BASE TABLE"},
        {"engine", "InnoDB"},
        {"rowFormat", "Dynamic"},
        {"rows", RowEstimate},
        {"avgRowLength", AvgRowBytes},
        {"dataLength", DataBytes},
        {"indexLength", IndexBytes},
        {"autoIncrement", AutoIncrement},
        {"collation", Collation},
        {"created", "2026-01-02 03:04"},
        {"updated", "2026-02-03 04:05"},
        {"comment", "order headers"},
    };
}

QJsonArray columnRows()
{
    return QJsonArray{
        QJsonObject{
            {"name", "id"},
            {"type", "bigint unsigned"},
            {"key", "PRI"},
            {"nullable", false},
            {"default", ""},
            {"extra", "auto_increment"},
            {"comment", ""},
        },
        QJsonObject{
            {"name", "customer_id"},
            {"type", "bigint unsigned"},
            {"key", "MUL"},
            {"nullable", true},
            {"default", ""},
            {"extra", ""},
            {"comment", "who ordered"},
        },
        QJsonObject{
            {"name", "total"},
            {"type", "decimal(10,2)"},
            {"key", ""},
            {"nullable", false},
            {"default", "0.00"},
            {"extra", ""},
            {"comment", ""},
        },
    };
}

QJsonArray indexRows()
{
    return QJsonArray{
        QJsonObject{
            {"name", "PRIMARY"},
            {"columns", "id"},
            {"unique", true},
            {"type", "BTREE"},
            {"cardinality", RowEstimate},
        },
        QJsonObject{
            {"name", "idx_customer"},
            {"columns", "customer_id, created_at"},
            {"unique", false},
            {"type", "BTREE"},
            {"cardinality", IndexCardinality},
        },
    };
}

QJsonArray foreignKeyRows()
{
    return QJsonArray{QJsonObject{
        {"name", "fk_orders_customer"},
        {"columns", "customer_id"},
        {"refSchema", Schema},
        {"refTable", "customers"},
        {"refColumns", "id"},
        {"onUpdate", "CASCADE"},
        {"onDelete", "RESTRICT"},
    }};
}

// What a schema holds: a table with sizes, and a view with none of them.
QJsonArray schemaTableRows()
{
    return QJsonArray{
        tableInfo(),
        QJsonObject{
            {"name", "order_view"},
            {"type", "VIEW"},
            {"engine", ""},
            {"rows", 0},
            {"dataLength", 0},
            {"indexLength", 0},
            {"collation", ""},
        },
    };
}

QTabWidget *tabsOf(const QWidget &view)
{
    return view.findChild<QTabWidget *>();
}

QStringList tabLabels(const QWidget &view)
{
    QStringList out;
    const QTabWidget *tabs = tabsOf(view);
    if (!tabs)
    {
        return out;
    }
    for (int i = 0; i < tabs->count(); ++i)
    {
        out << tabs->tabText(i);
    }
    return out;
}

int tabAt(const QWidget &view, const QString &label)
{
    const QTabWidget *tabs = tabsOf(view);
    if (!tabs)
    {
        return -1;
    }
    for (int i = 0; i < tabs->count(); ++i)
    {
        if (tabs->tabText(i) == label)
        {
            return i;
        }
    }
    return -1;
}

QWidget *pageAt(const QWidget &view, const char *label)
{
    QTabWidget *tabs = tabsOf(view);
    const int ix = tabAt(view, QString::fromLatin1(label));
    return tabs && ix >= 0 ? tabs->widget(ix) : nullptr;
}

QTableWidget *tableAt(const QWidget &view, const char *label)
{
    return qobject_cast<QTableWidget *>(pageAt(view, label));
}

// Visiting a tab is the only thing that loads its section.
bool openTab(const QWidget &view, const char *label)
{
    QTabWidget *tabs = tabsOf(view);
    const int ix = tabAt(view, QString::fromLatin1(label));
    if (!tabs || ix < 0)
    {
        return false;
    }
    tabs->setCurrentIndex(ix);
    return true;
}

QStringList headerLabels(const QTableWidget *t)
{
    QStringList out;
    for (int c = 0; c < t->columnCount(); ++c)
    {
        const QTableWidgetItem *item = t->horizontalHeaderItem(c);
        out << (item ? item->text() : QString());
    }
    return out;
}

int rowOfKey(const QTableWidget *t, const char *key)
{
    for (int r = 0; r < t->rowCount(); ++r)
    {
        const QTableWidgetItem *item = t->item(r, 0);
        if (item && item->text() == QLatin1String(key))
        {
            return r;
        }
    }
    return -1;
}

// The whole row as text, so a cell the loader never filled reads as empty
// rather than as the cell beside it.
QStringList rowFor(const QTableWidget *t, const char *key)
{
    const int row = rowOfKey(t, key);
    if (row < 0)
    {
        return {};
    }
    QStringList out;
    for (int c = 0; c < t->columnCount(); ++c)
    {
        const QTableWidgetItem *item = t->item(row, c);
        out << (item ? item->text() : QString());
    }
    return out;
}

QString valueFor(const QTableWidget *t, const char *key)
{
    const QStringList cells = rowFor(t, key);
    return cells.size() > 1 ? cells.at(1) : QString();
}

// numItem keeps the raw value in UserRole so a formatted cell still sorts by
// size; without it "512 KiB" would sort above "1.0 MiB".
qint64 sortValue(const QTableWidget *t, const char *key, int column)
{
    const int row = rowOfKey(t, key);
    if (row < 0)
    {
        return -1;
    }
    const QTableWidgetItem *item = t->item(row, column);
    return item ? item->data(Qt::UserRole).toLongLong() : -1;
}

int callsTo(const StubBackend &backend, const char *path)
{
    int seen = 0;
    for (const StubBackend::Request &req : backend.requests())
    {
        if (req.path == QLatin1String(path))
        {
            ++seen;
        }
    }
    return seen;
}

} // namespace

class TestInspectorView : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void theTabsAreTheSectionsTheTargetHas();
    void aSchemaHasNoStructureTabs();
    void eachSectionLabelsItsColumns_data();
    void eachSectionLabelsItsColumns();

    void eachSectionAsksForItsOwnCall_data();
    void eachSectionAsksForItsOwnCall();

    void theInfoSectionShowsTheTableStats();
    void theColumnsSectionShowsEveryColumn();
    void theIndexesSectionMarksTheUniqueOnes();
    void theForeignKeysSectionJoinsTheReference();
    void theDdlSectionShowsTheStatementVerbatim();
    void theSchemaInfoSectionNamesTheCharset();
    void theTablesSectionListsWhatTheSchemaHolds();

    void aSectionIsLoadedOnItsFirstVisitOnly();
    void anUnknownInitialSectionOpensOnInfo();

    void aFailedSectionShowsTheErrorAndRetries();
    void aFailedDdlLoadShowsTheMessageInThePane();
    void aReplyWithNoRowLeavesTheInfoTableEmpty();

private:
    StubBackend m_backend;
};

void TestInspectorView::initTestCase()
{
    theme::apply(theme::defaultApp, FontSize);

    // The counts go through "%L1" and the lengths through fmtBytes, both of
    // which read QLocale(): pin it rather than let the host group the
    // thousands its own way.
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
}

void TestInspectorView::init()
{
    m_backend.clearRequests();
    QVERIFY(m_backend.listening());
    api()->setEndpoint(m_backend.base(), QString());
}

void TestInspectorView::theTabsAreTheSectionsTheTargetHas()
{
    m_backend.replyWithResult(QJsonArray{tableInfo()});
    InspectorView view(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table),
        QString()
    );
    api()->flush(FlushMs);

    QCOMPARE(
        tabLabels(view), QStringList(
                             {QString::fromLatin1(InfoTab), QString::fromLatin1(ColumnsTab),
                              QString::fromLatin1(IndexesTab), QString::fromLatin1(ForeignKeysTab),
                              QString::fromLatin1(DdlTab)}
                         )
    );

    // The DDL is the one section that is not a table.
    QVERIFY(tableAt(view, DdlTab) == nullptr);
    QVERIFY(qobject_cast<QPlainTextEdit *>(pageAt(view, DdlTab)) != nullptr);
}

void TestInspectorView::aSchemaHasNoStructureTabs()
{
    // No table means no columns, indexes, FKs or DDL to show: the schema gets
    // the list of what it holds instead.
    m_backend.replyWithResult(QJsonObject{});
    InspectorView view(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString(), QString()
    );
    api()->flush(FlushMs);

    QCOMPARE(
        tabLabels(view), QStringList({QString::fromLatin1(InfoTab), QString::fromLatin1(TablesTab)})
    );
}

void TestInspectorView::eachSectionLabelsItsColumns_data()
{
    QTest::addColumn<QString>("table");
    QTest::addColumn<QString>("tab");
    QTest::addColumn<QStringList>("headers");

    QTest::newRow("table info") << QString::fromLatin1(Table) << QString::fromLatin1(InfoTab)
                                << QStringList({"Property", "Value"});
    QTest::newRow("columns"
    ) << QString::fromLatin1(Table)
      << QString::fromLatin1(ColumnsTab)
      << QStringList({"Column", "Type", "Key", "Null", "Default", "Extra", "Comment"});
    QTest::newRow("indexes") << QString::fromLatin1(Table) << QString::fromLatin1(IndexesTab)
                             << QStringList({"Index", "Columns", "Unique", "Type", "Cardinality"});
    QTest::newRow("foreign keys"
    ) << QString::fromLatin1(Table)
      << QString::fromLatin1(ForeignKeysTab)
      << QStringList({"Name", "Columns", "References", "On Update", "On Delete"});
    QTest::newRow("schema tables")
        << QString() << QString::fromLatin1(TablesTab)
        << QStringList({"Table", "Type", "Engine", "Rows", "Data", "Index", "Collation"});
}

void TestInspectorView::eachSectionLabelsItsColumns()
{
    QFETCH(QString, table);
    QFETCH(QString, tab);
    QFETCH(QStringList, headers);

    // The pages are built by the constructor, so the headers are there before
    // any section has been visited.
    m_backend.replyWithResult(QJsonArray{});
    InspectorView view(QString::fromLatin1(ConnID), QString::fromLatin1(Schema), table, QString());
    api()->flush(FlushMs);

    const QTableWidget *t = tableAt(view, tab.toLatin1().constData());
    QVERIFY(t);
    QCOMPARE(headerLabels(t), headers);
}

void TestInspectorView::eachSectionAsksForItsOwnCall_data()
{
    QTest::addColumn<QString>("table");
    QTest::addColumn<QString>("section");
    QTest::addColumn<QString>("path");
    QTest::addColumn<QJsonArray>("args");

    QTest::newRow("table info") << QString::fromLatin1(Table) << QString::fromLatin1(InfoSection)
                                << QString::fromLatin1(TablesInfoPath) << tableArgs();
    QTest::newRow("columns") << QString::fromLatin1(Table) << QString::fromLatin1(ColumnsSection)
                             << QString::fromLatin1(ColumnsPath) << tableArgs();
    QTest::newRow("indexes") << QString::fromLatin1(Table) << QString::fromLatin1(IndexesSection)
                             << QString::fromLatin1(IndexesPath) << tableArgs();
    QTest::newRow("foreign keys") << QString::fromLatin1(Table) << QString::fromLatin1(FksSection)
                                  << QString::fromLatin1(ForeignKeysPath) << tableArgs();
    QTest::newRow("ddl") << QString::fromLatin1(Table) << QString::fromLatin1(DdlSection)
                         << QString::fromLatin1(ShowCreatePath) << tableArgs();

    // A schema reads its own header from a different method, and its table
    // list from TablesInfo with the table left empty.
    QTest::newRow("schema info") << QString() << QString::fromLatin1(InfoSection)
                                 << QString::fromLatin1(SchemaInfoPath) << schemaArgs();
    QTest::newRow("schema tables") << QString() << QString::fromLatin1(TablesSection)
                                   << QString::fromLatin1(TablesInfoPath) << wholeSchemaArgs();
}

void TestInspectorView::eachSectionAsksForItsOwnCall()
{
    QFETCH(QString, table);
    QFETCH(QString, section);
    QFETCH(QString, path);
    QFETCH(QJsonArray, args);

    // Opening straight on a section is what the restored workspace does, and
    // it must cost exactly the one call that section needs.
    m_backend.replyWithResult(QJsonArray{});
    InspectorView view(QString::fromLatin1(ConnID), QString::fromLatin1(Schema), table, section);
    api()->flush(FlushMs);

    QCOMPARE(m_backend.requests().size(), 1);
    QCOMPARE(m_backend.requests().at(0).path, path);
    QCOMPARE(m_backend.requests().at(0).args, args);
}

void TestInspectorView::theInfoSectionShowsTheTableStats()
{
    m_backend.replyWithResult(QJsonArray{tableInfo()});
    InspectorView view(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table),
        QString::fromLatin1(InfoSection)
    );
    api()->flush(FlushMs);

    const QTableWidget *t = tableAt(view, InfoTab);
    QVERIFY(t);
    QCOMPARE(t->rowCount(), InfoRows);

    // The name is qualified from the schema the inspector was opened for, not
    // from the reply.
    QCOMPARE(valueFor(t, "Table"), QStringLiteral("shop.orders"));
    QCOMPARE(valueFor(t, "Type"), QStringLiteral("BASE TABLE"));
    QCOMPARE(valueFor(t, "Engine"), QStringLiteral("InnoDB"));
    QCOMPARE(valueFor(t, "Row Format"), QStringLiteral("Dynamic"));
    QCOMPARE(valueFor(t, "Rows (estimate)"), QStringLiteral("1,234,567"));
    QCOMPARE(valueFor(t, "Avg Row Length"), QStringLiteral("1.5 KiB"));
    QCOMPARE(valueFor(t, "Data Length"), QStringLiteral("1.0 MiB"));
    QCOMPARE(valueFor(t, "Index Length"), QStringLiteral("512 KiB"));

    // An auto-increment value is an identifier rather than a magnitude, so it
    // is the one number here with no group separators.
    QCOMPARE(valueFor(t, "Auto Increment"), QStringLiteral("90210"));

    QCOMPARE(valueFor(t, "Collation"), QString::fromLatin1(Collation));
    QCOMPARE(valueFor(t, "Created"), QStringLiteral("2026-01-02 03:04"));
    QCOMPARE(valueFor(t, "Updated"), QStringLiteral("2026-02-03 04:05"));
}

void TestInspectorView::theColumnsSectionShowsEveryColumn()
{
    m_backend.replyWithResult(columnRows());
    InspectorView view(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table),
        QString::fromLatin1(ColumnsSection)
    );
    api()->flush(FlushMs);

    const QTableWidget *t = tableAt(view, ColumnsTab);
    QVERIFY(t);
    QCOMPARE(t->rowCount(), columnRows().size());

    // nullable is a bool on the wire and the column header says "Null": the
    // cell has to read as the YES/NO the server itself would print.
    QCOMPARE(
        rowFor(t, "id"),
        QStringList({"id", "bigint unsigned", "PRI", "NO", "", "auto_increment", ""})
    );
    QCOMPARE(
        rowFor(t, "customer_id"),
        QStringList({"customer_id", "bigint unsigned", "MUL", "YES", "", "", "who ordered"})
    );
    QCOMPARE(rowFor(t, "total"), QStringList({"total", "decimal(10,2)", "", "NO", "0.00", "", ""}));
}

void TestInspectorView::theIndexesSectionMarksTheUniqueOnes()
{
    m_backend.replyWithResult(indexRows());
    InspectorView view(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table),
        QString::fromLatin1(IndexesSection)
    );
    api()->flush(FlushMs);

    const QTableWidget *t = tableAt(view, IndexesTab);
    QVERIFY(t);
    QCOMPARE(t->rowCount(), indexRows().size());

    // A non-unique index leaves the cell empty rather than saying "NO": the
    // column is a marker, not an answer.
    QCOMPARE(rowFor(t, "PRIMARY"), QStringList({"PRIMARY", "id", "YES", "BTREE", "1,234,567"}));
    QCOMPARE(
        rowFor(t, "idx_customer"),
        QStringList({"idx_customer", "customer_id, created_at", "", "BTREE", "4,096"})
    );

    const int cardinalityColumn = 4;
    QCOMPARE(sortValue(t, "PRIMARY", cardinalityColumn), qint64(RowEstimate));
    QCOMPARE(sortValue(t, "idx_customer", cardinalityColumn), qint64(IndexCardinality));
}

void TestInspectorView::theForeignKeysSectionJoinsTheReference()
{
    m_backend.replyWithResult(foreignKeyRows());
    InspectorView view(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table),
        QString::fromLatin1(FksSection)
    );
    api()->flush(FlushMs);

    const QTableWidget *t = tableAt(view, ForeignKeysTab);
    QVERIFY(t);
    QCOMPARE(t->rowCount(), foreignKeyRows().size());

    // The reply carries schema, table and columns apart; one cell is what the
    // user reads, so the join is the behaviour.
    QCOMPARE(
        rowFor(t, "fk_orders_customer"),
        QStringList(
            {"fk_orders_customer", "customer_id", "shop.customers (id)", "CASCADE", "RESTRICT"}
        )
    );
}

void TestInspectorView::theDdlSectionShowsTheStatementVerbatim()
{
    m_backend.replyWithResult(QString::fromLatin1(Ddl));
    InspectorView view(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table),
        QString::fromLatin1(DdlSection)
    );
    api()->flush(FlushMs);

    auto *pane = qobject_cast<QPlainTextEdit *>(pageAt(view, DdlTab));
    QVERIFY(pane);
    QCOMPARE(pane->toPlainText(), QString::fromLatin1(Ddl));

    // The inspector reads a server; nothing on this page is an edit box.
    QVERIFY(pane->isReadOnly());
}

void TestInspectorView::theSchemaInfoSectionNamesTheCharset()
{
    m_backend.replyWithResult(QJsonObject{{"charset", "utf8mb4"}, {"collation", Collation}});
    InspectorView view(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString(),
        QString::fromLatin1(InfoSection)
    );
    api()->flush(FlushMs);

    const QTableWidget *t = tableAt(view, InfoTab);
    QVERIFY(t);

    const int schemaInfoRows = 3;
    QCOMPARE(t->rowCount(), schemaInfoRows);
    QCOMPARE(valueFor(t, "Schema"), QString::fromLatin1(Schema));
    QCOMPARE(valueFor(t, "Charset"), QStringLiteral("utf8mb4"));
    QCOMPARE(valueFor(t, "Collation"), QString::fromLatin1(Collation));
}

void TestInspectorView::theTablesSectionListsWhatTheSchemaHolds()
{
    m_backend.replyWithResult(schemaTableRows());
    InspectorView view(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString(),
        QString::fromLatin1(TablesSection)
    );
    api()->flush(FlushMs);

    const QTableWidget *t = tableAt(view, TablesTab);
    QVERIFY(t);
    QCOMPARE(t->rowCount(), schemaTableRows().size());

    QCOMPARE(
        rowFor(t, "orders"), QStringList(
                                 {"orders", "BASE TABLE", "InnoDB", "1,234,567", "1.0 MiB",
                                  "512 KiB", "utf8mb4_0900_ai_ci"}
                             )
    );

    // A view has no engine, no storage and no collation of its own, and the
    // sizes must read as nothing rather than be left blank.
    QCOMPARE(
        rowFor(t, "order_view"), QStringList({"order_view", "VIEW", "", "0", "0 B", "0 B", ""})
    );

    const int dataColumn = 4;
    QCOMPARE(sortValue(t, "orders", dataColumn), qint64(DataBytes));
}

void TestInspectorView::aSectionIsLoadedOnItsFirstVisitOnly()
{
    m_backend.replyWithResult(QJsonArray{tableInfo()});
    InspectorView view(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table),
        QString()
    );
    api()->flush(FlushMs);

    const QTableWidget *info = tableAt(view, InfoTab);
    const QTableWidget *columns = tableAt(view, ColumnsTab);
    QVERIFY(info);
    QVERIFY(columns);
    QCOMPARE(m_backend.requests().size(), 1);
    QCOMPARE(columns->rowCount(), 0);

    m_backend.replyWithResult(columnRows());
    QVERIFY(openTab(view, ColumnsTab));
    api()->flush(FlushMs);

    QCOMPARE(callsTo(m_backend, ColumnsPath), 1);
    QCOMPARE(m_backend.requests().last().args, tableArgs());
    QCOMPARE(columns->rowCount(), columnRows().size());

    // Each section owns its own page, so the rows of the one left behind are
    // off screen rather than under the new section's headers.
    QCOMPARE(tabsOf(view)->currentWidget(), columns);
    QCOMPARE(info->rowCount(), InfoRows);

    // Going back and forth is free: a loaded section is never re-fetched, so
    // the tab strip stays usable over a slow link.
    QVERIFY(openTab(view, InfoTab));
    QVERIFY(openTab(view, ColumnsTab));
    api()->flush(FlushMs);
    QCOMPARE(callsTo(m_backend, TablesInfoPath), 1);
    QCOMPARE(callsTo(m_backend, ColumnsPath), 1);
}

void TestInspectorView::anUnknownInitialSectionOpensOnInfo()
{
    // The section id comes off disk, so it can name a section this target does
    // not have: a schema restored as "columns", or an id from an older build.
    m_backend.replyWithResult(QJsonArray{tableInfo()});
    InspectorView view(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table),
        QStringLiteral("no-such-section")
    );
    api()->flush(FlushMs);

    QCOMPARE(tabsOf(view)->currentIndex(), tabAt(view, QString::fromLatin1(InfoTab)));
    QCOMPARE(m_backend.requests().size(), 1);
    QCOMPARE(m_backend.requests().at(0).path, QString::fromLatin1(TablesInfoPath));

    m_backend.clearRequests();
    m_backend.replyWithResult(QJsonObject{});
    InspectorView schema(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString(),
        QString::fromLatin1(ColumnsSection)
    );
    api()->flush(FlushMs);

    QCOMPARE(tabsOf(schema)->currentIndex(), 0);
    QCOMPARE(m_backend.requests().size(), 1);
    QCOMPARE(m_backend.requests().at(0).path, QString::fromLatin1(SchemaInfoPath));
}

void TestInspectorView::aFailedSectionShowsTheErrorAndRetries()
{
    m_backend.replyWithError(QString::fromLatin1(ColumnsDenied));
    InspectorView view(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table),
        QString::fromLatin1(ColumnsSection)
    );
    api()->flush(FlushMs);

    QTableWidget *columns = tableAt(view, ColumnsTab);
    QVERIFY(columns);

    // An empty table here would read as a table with no columns at all, which
    // is a thing that cannot exist.
    QCOMPARE(columns->rowCount(), 1);
    QVERIFY(columns->item(0, 0));
    QCOMPARE(
        columns->item(0, 0)->text(),
        QStringLiteral("error: %1").arg(QString::fromLatin1(ColumnsDenied))
    );

    m_backend.replyWithResult(QJsonArray{tableInfo()});
    QVERIFY(openTab(view, InfoTab));
    api()->flush(FlushMs);

    // The failure released the section, so the next visit asks again rather
    // than leaving the tab showing an error for the rest of the session.
    m_backend.replyWithResult(columnRows());
    QVERIFY(openTab(view, ColumnsTab));
    api()->flush(FlushMs);

    QCOMPARE(callsTo(m_backend, ColumnsPath), 2);
    QCOMPARE(columns->rowCount(), columnRows().size());
    QVERIFY(rowOfKey(columns, "id") >= 0);
}

void TestInspectorView::aFailedDdlLoadShowsTheMessageInThePane()
{
    m_backend.replyWithError(QString::fromLatin1(DdlDenied));
    InspectorView view(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table),
        QString::fromLatin1(DdlSection)
    );
    api()->flush(FlushMs);

    auto *pane = qobject_cast<QPlainTextEdit *>(pageAt(view, DdlTab));
    QVERIFY(pane);
    QCOMPARE(pane->toPlainText(), QString::fromLatin1(DdlDenied));

    m_backend.replyWithResult(QJsonArray{tableInfo()});
    QVERIFY(openTab(view, InfoTab));
    api()->flush(FlushMs);

    m_backend.replyWithResult(QString::fromLatin1(Ddl));
    QVERIFY(openTab(view, DdlTab));
    api()->flush(FlushMs);

    QCOMPARE(callsTo(m_backend, ShowCreatePath), 2);
    QCOMPARE(pane->toPlainText(), QString::fromLatin1(Ddl));
}

void TestInspectorView::aReplyWithNoRowLeavesTheInfoTableEmpty()
{
    // information_schema has no row for a table that was dropped between the
    // sidebar listing it and the inspector opening on it.
    m_backend.replyWithResult(QJsonArray{});
    InspectorView view(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table),
        QString::fromLatin1(InfoSection)
    );
    api()->flush(FlushMs);

    const QTableWidget *t = tableAt(view, InfoTab);
    QVERIFY(t);
    QCOMPARE(t->rowCount(), 0);
    QCOMPARE(headerLabels(t), QStringList({"Property", "Value"}));
}

QTEST_MAIN(TestInspectorView)

#include "tst_inspectorview.moc"
