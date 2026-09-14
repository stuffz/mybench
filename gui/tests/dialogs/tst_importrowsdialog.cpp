#include "dialogs/importrowsdialog.h"

#include "app/api.h"
#include "app/stubbackend.h"
#include "app/theme.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLatin1String>
#include <QList>
#include <QLocale>
#include <QObject>
#include <QPushButton>
#include <QString>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTest>
#include <functional>

namespace
{

constexpr auto ColumnsPath = "/rpc/admin/Columns";
constexpr auto ImportPath = "/rpc/query/ImportRows";
constexpr auto ReadCsvPath = "/rpc/query/ReadCSV";

constexpr auto ConnID = "c1";
constexpr auto Schema = "app";
constexpr auto Table = "users";

constexpr int ColCount = 3;

QJsonObject column(const QString &name, const QString &type, const QString &extra = {})
{
    QJsonObject col;
    col.insert(QStringLiteral("name"), name);
    col.insert(QStringLiteral("type"), type);
    col.insert(QStringLiteral("extra"), extra);
    return col;
}

// The target table as admin.Columns describes it. "note" is the column no row
// below ever fills, so the INSERT can be seen leaving it out.
QJsonArray tableColumns()
{
    return {
        column(QStringLiteral("id"), QStringLiteral("int"), QStringLiteral("auto_increment")),
        column(QStringLiteral("name"), QStringLiteral("varchar(64)")),
        column(QStringLiteral("note"), QStringLiteral("text")),
    };
}

// Spins the event loop until the condition holds, or gives up. A loop rather
// than QTRY_VERIFY, which cannot be used from a helper: it returns from its
// own function on failure, which here would only skip the wait.
bool waitUntil(const std::function<bool()> &done)
{
    for (int waited = 0; waited < 5000; waited += 10)
    {
        if (done())
        {
            return true;
        }
        QTest::qWait(10);
    }
    return false;
}

// The calls that went to one RPC method. Every dialog asks for its columns as
// it is built, so an assertion about the import picks out its own conversation.
QList<StubBackend::Request> callsTo(const StubBackend &backend, const char *path)
{
    QList<StubBackend::Request> out;
    for (const StubBackend::Request &req : backend.requests())
    {
        if (req.path == QLatin1String(path))
        {
            out.append(req);
        }
    }
    return out;
}

QTableWidget *grid(const ImportRowsDialog &dlg)
{
    return dlg.findChild<QTableWidget *>();
}

QPushButton *button(const ImportRowsDialog &dlg, const QString &text)
{
    for (QPushButton *b : dlg.findChildren<QPushButton *>())
    {
        if (b->text() == text)
        {
            return b;
        }
    }
    return nullptr;
}

// Not found by text: the Insert button's own label counts the rows, so it
// changes under every edit.
QPushButton *insertButton(const ImportRowsDialog &dlg)
{
    auto *box = dlg.findChild<QDialogButtonBox *>();
    if (!box)
    {
        return nullptr;
    }
    for (QAbstractButton *b : box->buttons())
    {
        if (box->buttonRole(b) == QDialogButtonBox::AcceptRole)
        {
            return qobject_cast<QPushButton *>(b);
        }
    }
    return nullptr;
}

// The hint below the grid is a mutedLabel; the error line is the other one.
QLabel *errorLabel(const ImportRowsDialog &dlg)
{
    for (QLabel *l : dlg.findChildren<QLabel *>())
    {
        if (!l->property("muted").toBool())
        {
            return l;
        }
    }
    return nullptr;
}

QCheckBox *checkBox(const ImportRowsDialog &dlg, const QString &text)
{
    for (QCheckBox *c : dlg.findChildren<QCheckBox *>())
    {
        if (c->text() == text)
        {
            return c;
        }
    }
    return nullptr;
}

// What committing a cell editor does, and the path the Insert button's count
// hangs off. False when the cell has no item to type into.
bool typeInto(QTableWidget *g, int row, int col, const QString &text)
{
    QTableWidgetItem *cell = g->item(row, col);
    if (!cell)
    {
        return false;
    }
    cell->setText(text);
    return true;
}

} // namespace

// This dialog writes rows into a real table, so most of what is pinned here is
// the ImportRows payload: which columns it names, which rows it leaves out, and
// how an empty cell is spelled. The CSV half is a backend parse (ReadCSV) fed
// back into the grid, and only its guard is reachable without the modal file
// chooser.
class TestImportRowsDialog : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void theColumnsCallNamesTheTargetTable();
    void theColumnsReplyBuildsTheGridAndItsFirstRow();
    void aColumnsFailureIsShownAndLeavesTheGridEmpty();
    void addRowGivesEveryCellOfTheNewRowAnItem();
    void removeSelectedRowsDropsEachSelectedRowOnce();
    void theInsertButtonFollowsTheFilledRowCount();
    void aHandTypedRowIsSentAsPositionalArgs();
    void anEmptyRowIsSkippedAndAnEmptyCellIsNull();
    void anEmptyCellIsSentAsTextWhenNullIsUnchecked();
    void aSuccessfulImportReportsTheRowsItInserted();
    void aFailedImportSurfacesTheErrorAndInsertsNothing();
    void theCsvOptionsAreIdleUntilAFileIsLoaded();

private:
    StubBackend m_backend;
};

void TestImportRowsDialog::initTestCase()
{
    // The error line is coloured from theme::current() while the dialog is
    // being constructed, so a palette has to be installed before the first one.
    theme::apply(theme::defaultApp, 13);
    // The Insert button counts its rows through %L1, so pin the locale rather
    // than let the machine's decide what the label says.
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
}

void TestImportRowsDialog::init()
{
    QVERIFY(m_backend.listening());
    api()->setEndpoint(m_backend.base(), QString());
    m_backend.clearRequests();
    // The reply the constructor's Columns call gets. A slot that wants another
    // one sets it before it builds its dialog.
    m_backend.replyWithResult(tableColumns());
}

void TestImportRowsDialog::cleanup()
{
    // A dialog destroyed as its slot returns can still have a reply in flight;
    // draining it here keeps it out of the next slot's requests.
    api()->flush(2000);
}

void TestImportRowsDialog::theColumnsCallNamesTheTargetTable()
{
    ImportRowsDialog dlg(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table)
    );

    QVERIFY(waitUntil([this] { return callsTo(m_backend, ColumnsPath).size() == 1; }));
    const StubBackend::Request req = callsTo(m_backend, ColumnsPath).at(0);
    QCOMPARE(req.verb, QStringLiteral("POST"));
    QCOMPARE(
        req.args,
        QJsonArray(
            {QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table)}
        )
    );

    // Nothing else goes out by itself: the parse waits for a file and the
    // insert waits for the button.
    QCOMPARE(m_backend.requests().size(), 1);
}

void TestImportRowsDialog::theColumnsReplyBuildsTheGridAndItsFirstRow()
{
    ImportRowsDialog dlg(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table)
    );
    QTableWidget *g = grid(dlg);
    QVERIFY(g);
    QVERIFY(waitUntil([g] { return g->columnCount() == ColCount; }));

    QCOMPARE(g->horizontalHeaderItem(0)->text(), QStringLiteral("id"));
    QCOMPARE(g->horizontalHeaderItem(1)->text(), QStringLiteral("name"));
    QCOMPARE(g->horizontalHeaderItem(2)->text(), QStringLiteral("note"));

    // The type, and the extra when there is one, live in the tooltip: a header
    // wide enough for "varchar(64)" would cost the value it labels.
    QCOMPARE(g->horizontalHeaderItem(0)->toolTip(), QStringLiteral("int auto_increment"));
    QCOMPARE(g->horizontalHeaderItem(1)->toolTip(), QStringLiteral("varchar(64)"));
    QCOMPARE(g->horizontalHeaderItem(2)->toolTip(), QStringLiteral("text"));

    // One row, items and all, is waiting as soon as the columns land.
    QCOMPARE(g->rowCount(), 1);
    for (int c = 0; c < ColCount; ++c)
    {
        QVERIFY(g->item(0, c));
    }
}

void TestImportRowsDialog::aColumnsFailureIsShownAndLeavesTheGridEmpty()
{
    m_backend.replyWithError(QStringLiteral("unknown table 'nope'"));

    ImportRowsDialog dlg(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QStringLiteral("nope")
    );
    QLabel *error = errorLabel(dlg);
    QVERIFY(error);

    // The dialog is never shown, so the label's own hidden flag is what says
    // whether the message would be on screen.
    QVERIFY(error->isHidden());
    QVERIFY(waitUntil([error] { return !error->isHidden(); }));
    QCOMPARE(error->text(), QStringLiteral("unknown table 'nope'"));

    QTableWidget *g = grid(dlg);
    QVERIFY(g);
    QCOMPARE(g->columnCount(), 0);
    QCOMPARE(g->rowCount(), 0);

    QPushButton *insert = insertButton(dlg);
    QVERIFY(insert);
    QVERIFY(!insert->isEnabled());
}

void TestImportRowsDialog::addRowGivesEveryCellOfTheNewRowAnItem()
{
    ImportRowsDialog dlg(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table)
    );
    QTableWidget *g = grid(dlg);
    QVERIFY(g);
    QVERIFY(waitUntil([g] { return g->columnCount() == ColCount; }));

    QPushButton *add = button(dlg, QStringLiteral("Add Row"));
    QVERIFY(add);
    add->click();

    QCOMPARE(g->rowCount(), 2);
    for (int c = 0; c < ColCount; ++c)
    {
        QVERIFY(g->item(1, c));
        QVERIFY(g->item(1, c)->text().isEmpty());
    }
}

void TestImportRowsDialog::removeSelectedRowsDropsEachSelectedRowOnce()
{
    ImportRowsDialog dlg(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table)
    );
    QTableWidget *g = grid(dlg);
    QVERIFY(g);
    QVERIFY(waitUntil([g] { return g->columnCount() == ColCount; }));

    QPushButton *add = button(dlg, QStringLiteral("Add Row"));
    QPushButton *remove = button(dlg, QStringLiteral("Remove Selected"));
    QVERIFY(add);
    QVERIFY(remove);
    add->click();
    add->click();
    QCOMPARE(g->rowCount(), 3);
    QVERIFY(typeInto(g, 0, 0, QStringLiteral("first")));
    QVERIFY(typeInto(g, 1, 0, QStringLiteral("second")));
    QVERIFY(typeInto(g, 2, 0, QStringLiteral("third")));

    g->item(0, 0)->setSelected(true);
    g->item(2, 1)->setSelected(true);
    remove->click();

    // Bottom row first: taking row 0 out ahead of row 2 would renumber row 2
    // out from under the removal that follows.
    QCOMPARE(g->rowCount(), 1);
    QCOMPARE(g->item(0, 0)->text(), QStringLiteral("second"));

    // Two selected cells in one row are still one row to remove.
    g->item(0, 0)->setSelected(true);
    g->item(0, 1)->setSelected(true);
    remove->click();

    QCOMPARE(g->rowCount(), 0);
    QPushButton *insert = insertButton(dlg);
    QVERIFY(insert);
    QVERIFY(!insert->isEnabled());
}

void TestImportRowsDialog::theInsertButtonFollowsTheFilledRowCount()
{
    ImportRowsDialog dlg(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table)
    );
    QTableWidget *g = grid(dlg);
    QVERIFY(g);
    QVERIFY(waitUntil([g] { return g->columnCount() == ColCount; }));

    QPushButton *insert = insertButton(dlg);
    QVERIFY(insert);
    QVERIFY(!insert->isEnabled());
    QCOMPARE(insert->text(), QStringLiteral("Insert 0 Rows"));

    QVERIFY(typeInto(g, 0, 1, QStringLiteral("ada")));
    QVERIFY(insert->isEnabled());
    QCOMPARE(insert->text(), QStringLiteral("Insert 1 Row"));

    QPushButton *add = button(dlg, QStringLiteral("Add Row"));
    QVERIFY(add);
    add->click();
    QCOMPARE(insert->text(), QStringLiteral("Insert 1 Row"));

    QVERIFY(typeInto(g, 1, 0, QStringLiteral("2")));
    QCOMPARE(insert->text(), QStringLiteral("Insert 2 Rows"));

    // Emptying a row's last filled cell takes the row back out of the count,
    // and an empty grid has nothing to send.
    QVERIFY(typeInto(g, 0, 1, QString()));
    QVERIFY(typeInto(g, 1, 0, QString()));
    QCOMPARE(insert->text(), QStringLiteral("Insert 0 Rows"));
    QVERIFY(!insert->isEnabled());
}

void TestImportRowsDialog::aHandTypedRowIsSentAsPositionalArgs()
{
    ImportRowsDialog dlg(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table)
    );
    QTableWidget *g = grid(dlg);
    QVERIFY(g);
    QVERIFY(waitUntil([g] { return g->columnCount() == ColCount; }));

    QVERIFY(typeInto(g, 0, 0, QStringLiteral("1")));
    QVERIFY(typeInto(g, 0, 1, QStringLiteral("ada")));

    m_backend.replyWithResult(QJsonValue(1));
    m_backend.clearRequests();
    QPushButton *insert = insertButton(dlg);
    QVERIFY(insert);
    insert->click();

    QVERIFY(waitUntil([this] { return callsTo(m_backend, ImportPath).size() == 1; }));
    const StubBackend::Request req = callsTo(m_backend, ImportPath).at(0);
    QCOMPARE(req.verb, QStringLiteral("POST"));

    // Nobody typed into "note", so it is left out of the INSERT entirely and
    // the column keeps whatever default the server gives it.
    QCOMPARE(
        req.args, QJsonArray({
                      QString::fromLatin1(ConnID),
                      QString::fromLatin1(Schema),
                      QString::fromLatin1(Table),
                      QJsonArray({QStringLiteral("id"), QStringLiteral("name")}),
                      QJsonArray({QJsonArray({QStringLiteral("1"), QStringLiteral("ada")})}),
                  })
    );
}

void TestImportRowsDialog::anEmptyRowIsSkippedAndAnEmptyCellIsNull()
{
    ImportRowsDialog dlg(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table)
    );
    QTableWidget *g = grid(dlg);
    QVERIFY(g);
    QVERIFY(waitUntil([g] { return g->columnCount() == ColCount; }));

    QPushButton *add = button(dlg, QStringLiteral("Add Row"));
    QVERIFY(add);
    add->click();
    add->click();
    QVERIFY(typeInto(g, 0, 0, QStringLiteral("1")));
    QVERIFY(typeInto(g, 0, 1, QStringLiteral("ada")));
    QVERIFY(typeInto(g, 2, 0, QStringLiteral("2")));

    m_backend.replyWithResult(QJsonValue(2));
    m_backend.clearRequests();
    QPushButton *insert = insertButton(dlg);
    QVERIFY(insert);
    QCOMPARE(insert->text(), QStringLiteral("Insert 2 Rows"));
    insert->click();

    QVERIFY(waitUntil([this] { return callsTo(m_backend, ImportPath).size() == 1; }));
    const StubBackend::Request req = callsTo(m_backend, ImportPath).at(0);
    QCOMPARE(req.args.at(3).toArray(), QJsonArray({QStringLiteral("id"), QStringLiteral("name")}));

    // The untouched middle row is not a row of NULLs to insert, and the empty
    // "name" of the row that was typed into is a NULL rather than an empty
    // string, which is what the checked box promises.
    QCOMPARE(
        req.args.at(4).toArray(),
        QJsonArray({
            QJsonArray({QStringLiteral("1"), QStringLiteral("ada")}),
            QJsonArray({QStringLiteral("2"), QJsonValue(QJsonValue::Null)}),
        })
    );
}

void TestImportRowsDialog::anEmptyCellIsSentAsTextWhenNullIsUnchecked()
{
    ImportRowsDialog dlg(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table)
    );
    QTableWidget *g = grid(dlg);
    QVERIFY(g);
    QVERIFY(waitUntil([g] { return g->columnCount() == ColCount; }));

    QCheckBox *nulls = checkBox(dlg, QStringLiteral("Empty Means NULL"));
    QVERIFY(nulls);
    QVERIFY(nulls->isChecked());
    nulls->setChecked(false);

    QPushButton *add = button(dlg, QStringLiteral("Add Row"));
    QVERIFY(add);
    add->click();
    QVERIFY(typeInto(g, 0, 0, QStringLiteral("1")));
    QVERIFY(typeInto(g, 0, 1, QStringLiteral("ada")));
    QVERIFY(typeInto(g, 1, 0, QStringLiteral("2")));

    m_backend.replyWithResult(QJsonValue(2));
    m_backend.clearRequests();
    QPushButton *insert = insertButton(dlg);
    QVERIFY(insert);
    insert->click();

    QVERIFY(waitUntil([this] { return callsTo(m_backend, ImportPath).size() == 1; }));
    const StubBackend::Request req = callsTo(m_backend, ImportPath).at(0);

    // The same grid as the NULL case: unchecked, the empty cell carries the
    // empty string, which is a different value in a NOT NULL column.
    QCOMPARE(
        req.args.at(4).toArray(), QJsonArray({
                                      QJsonArray({QStringLiteral("1"), QStringLiteral("ada")}),
                                      QJsonArray({QStringLiteral("2"), QString()}),
                                  })
    );
}

void TestImportRowsDialog::aSuccessfulImportReportsTheRowsItInserted()
{
    ImportRowsDialog dlg(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table)
    );
    QTableWidget *g = grid(dlg);
    QVERIFY(g);
    QVERIFY(waitUntil([g] { return g->columnCount() == ColCount; }));
    QCOMPARE(dlg.insertedCount(), 0);

    QVERIFY(typeInto(g, 0, 0, QStringLiteral("1")));
    m_backend.replyWithResult(QJsonValue(2));
    QPushButton *insert = insertButton(dlg);
    QVERIFY(insert);
    insert->click();

    // Accepting is earned by the reply, not by the click, and the button is
    // held down in the meantime so the rows cannot be sent twice.
    QCOMPARE(dlg.result(), int(QDialog::Rejected));
    QVERIFY(!insert->isEnabled());

    QVERIFY(waitUntil([&dlg] { return dlg.result() == QDialog::Accepted; }));

    // The caller reports this number to the user, so it is what ImportRows
    // answered and not the number of rows the grid sent.
    QCOMPARE(dlg.insertedCount(), 2);

    QLabel *error = errorLabel(dlg);
    QVERIFY(error);
    QVERIFY(error->isHidden());
}

void TestImportRowsDialog::aFailedImportSurfacesTheErrorAndInsertsNothing()
{
    ImportRowsDialog dlg(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table)
    );
    QTableWidget *g = grid(dlg);
    QVERIFY(g);
    QVERIFY(waitUntil([g] { return g->columnCount() == ColCount; }));

    QVERIFY(typeInto(g, 0, 0, QStringLiteral("1")));
    QVERIFY(typeInto(g, 0, 1, QStringLiteral("ada")));
    m_backend.replyWithError(QStringLiteral("Duplicate entry '1' for key 'PRIMARY'"));

    QLabel *error = errorLabel(dlg);
    QPushButton *insert = insertButton(dlg);
    QVERIFY(error);
    QVERIFY(insert);
    insert->click();

    QVERIFY(waitUntil([error] { return !error->isHidden(); }));
    QCOMPARE(error->text(), QStringLiteral("Duplicate entry '1' for key 'PRIMARY'"));

    // The transaction rolled back, so nothing was written and nothing may be
    // reported as written. The grid keeps the rows for the user to fix.
    QCOMPARE(dlg.insertedCount(), 0);
    QCOMPARE(dlg.result(), int(QDialog::Rejected));
    QCOMPARE(g->item(0, 1)->text(), QStringLiteral("ada"));
    QVERIFY(insert->isEnabled());
    QCOMPARE(insert->text(), QStringLiteral("Insert 1 Row"));
}

void TestImportRowsDialog::theCsvOptionsAreIdleUntilAFileIsLoaded()
{
    ImportRowsDialog dlg(
        QString::fromLatin1(ConnID), QString::fromLatin1(Schema), QString::fromLatin1(Table)
    );
    QTableWidget *g = grid(dlg);
    QVERIFY(g);
    QVERIFY(waitUntil([g] { return g->columnCount() == ColCount; }));

    QComboBox *sep = dlg.findChild<QComboBox *>();
    QCheckBox *header = checkBox(dlg, QStringLiteral("Header Row"));
    QVERIFY(sep);
    QVERIFY(header);

    // Both describe how to parse a file, so they say nothing until there is
    // one to parse.
    QVERIFY(!sep->isEnabled());
    QVERIFY(!header->isEnabled());
    QVERIFY(!header->isChecked());

    // The data, not the label, is what ReadCSV is given as its separator.
    QCOMPARE(sep->count(), 4);
    QCOMPARE(sep->currentData().toString(), QStringLiteral(","));
    QCOMPARE(sep->itemData(1).toString(), QStringLiteral(";"));
    QCOMPARE(sep->itemData(2).toString(), QStringLiteral("\t"));
    QCOMPARE(sep->itemData(3).toString(), QStringLiteral("|"));

    m_backend.clearRequests();
    sep->setCurrentIndex(1);
    header->setChecked(true);
    api()->flush(2000);
    QTest::qWait(50);

    // Each choice re-parses the loaded file. With no file loaded there is
    // nothing to re-parse, and a ReadCSV on an empty path would fail in front
    // of a user who never asked for one.
    QVERIFY(callsTo(m_backend, ReadCsvPath).isEmpty());
    QVERIFY(m_backend.requests().isEmpty());
}

QTEST_MAIN(TestImportRowsDialog)

#include "tst_importrowsdialog.moc"
