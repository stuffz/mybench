#include "dialogs/aboutdialog.h"
#include "dialogs/querydialog.h"
#include "dialogs/shortcutsdialog.h"

#include "app/api.h"
#include "app/stubbackend.h"
#include "app/theme.h"
#include "dialogs/connectionsdialog.h"
#include "editor/sqlhighlighter.h"

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeySequence>
#include <QLabel>
#include <QLatin1String>
#include <QLayoutItem>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <QVector>
#include <QWidget>
#include <functional>

namespace
{

constexpr auto FormatPath = "/rpc/sqlfmt/Format";
constexpr auto AppInfoPath = "/rpc/admin/AppInfo";

// querydialog.cpp's own constant: the Format button never offers a choice.
constexpr int FormatTabWidth = 4;

constexpr int PollMs = 5;
constexpr int WaitMs = 5000;

// Spins the event loop until the condition holds, or gives up. A loop rather
// than QTRY_VERIFY, which cannot be used from a helper: it returns from its
// own function on failure, which here would only skip the wait.
bool waitUntil(const std::function<bool()> &done)
{
    for (int waited = 0; waited < WaitMs; waited += PollMs)
    {
        if (done())
        {
            return true;
        }
        QTest::qWait(PollMs);
    }
    return false;
}

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

// All three dialogs here are free functions that build a QDialog on the stack
// and end in exec(), so the only way to look at one is from inside its own
// event loop. The poll below waits for the modal to come up, hands it to the
// caller, and closes whatever the caller left open, which is what lets exec()
// return. Without it a failure would hang the run rather than report itself.
bool withModal(const std::function<void()> &open, const std::function<void(QDialog &)> &look)
{
    bool seen = false;
    QTimer poll;
    poll.setInterval(PollMs);
    QObject::connect(
        &poll, &QTimer::timeout, &poll,
        [&poll, &seen, &look]()
        {
            auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dlg)
            {
                return;
            }
            poll.stop();
            seen = true;
            look(*dlg);
            if (dlg->isVisible())
            {
                dlg->close();
            }
        }
    );
    poll.start();

    open();
    // About opens only once admin.AppInfo has answered, so its open() returns
    // long before there is a dialog to find.
    for (int waited = 0; waited < WaitMs && !seen; waited += PollMs)
    {
        QTest::qWait(PollMs);
    }
    return seen;
}

QPushButton *button(QDialog &dlg, const QString &text)
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

// The way out of each dialog: querydialog takes the standard Close button,
// whose label belongs to the platform, and About and Shortcuts add one of
// their own. The role is what they agree on.
QPushButton *roleButton(QDialog &dlg, QDialogButtonBox::ButtonRole role)
{
    auto *box = dlg.findChild<QDialogButtonBox *>();
    if (!box)
    {
        return nullptr;
    }
    for (QAbstractButton *b : box->buttons())
    {
        if (box->buttonRole(b) == role)
        {
            return qobject_cast<QPushButton *>(b);
        }
    }
    return nullptr;
}

// The one label the sheet sizes down: the format error in querydialog, the
// build stamps in About.
QLabel *smallText(QDialog &dlg)
{
    return dlg.findChild<QLabel *>(QStringLiteral("smallText"));
}

// One foreground per character, invalid where the highlighter left the text
// alone. The formats only reach the block layout once rehighlight() has run:
// with no view laying the document out, the contentsChange path
// QSyntaxHighlighter normally rides on leaves layout()->formats() empty.
QList<QColor> foregrounds(const QTextBlock &block)
{
    QList<QColor> colours(block.text().size(), QColor());
    for (const QTextLayout::FormatRange &range : block.layout()->formats())
    {
        for (int i = range.start; i < range.start + range.length; ++i)
        {
            colours[i] = range.format.foreground().color();
        }
    }
    return colours;
}

// Each credit is one row: the link, then the note beside it.
QVector<QPair<QString, QString>> credits(QDialog &dlg)
{
    QVector<QPair<QString, QString>> out;
    auto *root = qobject_cast<QVBoxLayout *>(dlg.layout());
    if (!root)
    {
        return out;
    }
    for (int i = 0; i < root->count(); ++i)
    {
        auto *rows = qobject_cast<QVBoxLayout *>(root->itemAt(i)->layout());
        if (!rows)
        {
            continue;
        }
        for (int r = 0; r < rows->count(); ++r)
        {
            auto *row = qobject_cast<QHBoxLayout *>(rows->itemAt(r)->layout());
            if (!row || row->count() < 2)
            {
                continue;
            }
            auto *link = qobject_cast<QLabel *>(row->itemAt(0)->widget());
            auto *note = qobject_cast<QLabel *>(row->itemAt(1)->widget());
            if (link && note)
            {
                out.append({link->text(), note->text()});
            }
        }
    }
    return out;
}

struct ShortcutRow
{
    QString keys;
    QString what;
};

struct ShortcutGroup
{
    QString title;
    QVector<ShortcutRow> rows;
};

QWidget *cellAt(QGridLayout *grid, int row, int column)
{
    QLayoutItem *item = grid->itemAtPosition(row, column);
    return item ? item->widget() : nullptr;
}

// Read in layout order: every group is a rule, then its muted title, then the
// grid holding its rows. The heading at the top of the dialog lives inside its
// own row layout, so it is never mistaken for a group title.
QVector<ShortcutGroup> shortcutGroups(QDialog &dlg)
{
    QVector<ShortcutGroup> out;
    auto *root = qobject_cast<QVBoxLayout *>(dlg.layout());
    if (!root)
    {
        return out;
    }
    for (int i = 0; i < root->count(); ++i)
    {
        QLayoutItem *item = root->itemAt(i);
        if (auto *title = qobject_cast<QLabel *>(item->widget()))
        {
            if (title->property("muted").toBool())
            {
                out.append({title->text(), {}});
            }
            continue;
        }
        auto *grid = qobject_cast<QGridLayout *>(item->layout());
        if (!grid || out.isEmpty())
        {
            continue;
        }
        for (int r = 0; r < grid->rowCount(); ++r)
        {
            auto *keys = qobject_cast<QLabel *>(cellAt(grid, r, 0));
            auto *what = qobject_cast<QLabel *>(cellAt(grid, r, 1));
            if (keys && what)
            {
                out.last().rows.append({keys->text(), what->text()});
            }
        }
    }
    return out;
}

QString native(const QKeySequence &seq)
{
    return seq.toString(QKeySequence::NativeText);
}

} // namespace

// The three dialogs with no state of their own: one statement, one build
// stamp, one table of bindings. Each is a free function ending in exec(), so
// every slot below drives it through withModal().
//
// What is pinned is what the user is told: the statement a connection is
// running, which build this is, and which key does what. The format round trip
// is the only backend conversation any of them has.
class TestSmallDialogs : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void theQueryDialogShowsTheStatementReadOnly();
    void theStatementIsSyntaxColoured();
    void theFormatButtonSendsTheStatementAndTabWidth();
    void aFormatFailureIsShownAndChangesNothing();
    void theCopyButtonTakesTheWholeStatement();
    void theQueryDialogClosesOnItsCloseButton();

    void theAboutBoxWaitsForTheBuildStamps();
    void theBuildLineJoinsVersionCommitAndDate();
    void aDevBuildWithNoStampsShowsNoBuildLine();
    void anUnparsableBuildDateIsShownAsItCame();
    void anAppInfoFailureTakesTheBuildLinesPlace();
    void theCreditsNameEveryProjectTheClientShips();

    void everyShortcutRowHasKeysAndADescription();
    void noTwoShortcutsClaimTheSameKeys();
    void theShortcutsAreGroupedTheWayTheUiIs();
    void theBindingsMatchTheOnesTheAppInstalls();
    void theShortcutsDialogTalksToNoBackend();

private:
    StubBackend m_backend;
};

void TestSmallDialogs::initTestCase()
{
    // querydialog styles its body from theme::currentEditor(), and both About
    // and Shortcuts render an icon from theme::current(), all while the dialog
    // is being built, so a palette has to be installed before the first one.
    theme::apply(theme::defaultApp, 13);
}

void TestSmallDialogs::init()
{
    QVERIFY(m_backend.listening());
    api()->setEndpoint(m_backend.base(), QString());
    m_backend.clearRequests();
}

void TestSmallDialogs::cleanup()
{
    // A dialog destroyed as its slot returns can still have a reply in flight;
    // draining it here keeps it out of the next slot's requests.
    api()->flush(2000);
}

void TestSmallDialogs::theQueryDialogShowsTheStatementReadOnly()
{
    // The processlist column shows one truncated line; this dialog is where
    // the rest of it goes, newlines and all.
    const QString sql = QStringLiteral("SELECT *\nFROM orders\nWHERE id = 7");
    const QString title = QStringLiteral("app@10.0.0.4 (thread 42)");

    QWidget parent;
    QString shownTitle;
    QString shownSql;
    bool readOnly = false;
    bool wraps = false;
    QStringList actions;
    const bool opened = withModal(
        [&parent, &title, &sql]() { showQueryDialog(&parent, title, sql); },
        [&](QDialog &dlg)
        {
            shownTitle = dlg.windowTitle();
            auto *body = dlg.findChild<QPlainTextEdit *>();
            if (body)
            {
                shownSql = body->toPlainText();
                readOnly = body->isReadOnly();
                wraps = body->lineWrapMode() == QPlainTextEdit::WidgetWidth;
            }
            auto *box = dlg.findChild<QDialogButtonBox *>();
            if (!box)
            {
                return;
            }
            for (QAbstractButton *b : box->buttons())
            {
                if (box->buttonRole(b) == QDialogButtonBox::ActionRole)
                {
                    actions << b->text();
                }
            }
            actions.sort();
        }
    );

    QVERIFY(opened);
    QCOMPARE(shownTitle, title);
    // Whole and unreflowed: the statement is evidence about a live connection,
    // so nothing may be trimmed out of it on the way to the screen.
    QCOMPARE(shownSql, sql);
    QVERIFY(readOnly);
    QVERIFY(wraps);
    // Sorted: what the box offers, not the order the style lays it out in.
    QCOMPARE(actions, QStringList({QStringLiteral("Copy"), QStringLiteral("Format")}));
}

void TestSmallDialogs::theStatementIsSyntaxColoured()
{
    const QString sql = QStringLiteral("SELECT x FROM t WHERE y = 'q'");

    QWidget parent;
    bool highlighted = false;
    QList<QColor> colours;
    const bool opened = withModal(
        [&parent, &sql]() { showQueryDialog(&parent, QStringLiteral("query"), sql); },
        [&](QDialog &dlg)
        {
            auto *body = dlg.findChild<QPlainTextEdit *>();
            if (!body)
            {
                return;
            }
            auto *highlighter = body->document()->findChild<SqlHighlighter *>();
            highlighted = highlighter != nullptr;
            if (highlighter)
            {
                highlighter->rehighlight();
            }
            colours = foregrounds(body->document()->findBlockByNumber(0));
        }
    );

    QVERIFY(opened);
    // The dialog owns the highlighter through the document it hands it.
    QVERIFY(highlighted);
    QCOMPARE(colours.size(), sql.size());

    // Which colour each token takes belongs to tst_sqlhighlighter; what
    // matters here is that the highlighter reached this document at all.
    const QColor keyword = colours.value(0);
    QVERIFY(keyword.isValid());
    QCOMPARE(colours.mid(0, 6), QList<QColor>(6, keyword));
    QCOMPARE(colours.mid(9, 4), QList<QColor>(4, keyword));

    // A bare identifier keeps the editor's own foreground, and the quoted
    // literal is a run of its own.
    QVERIFY(!colours.value(7).isValid());
    QVERIFY(colours.value(26).isValid());
    QVERIFY(colours.value(26) != keyword);
}

void TestSmallDialogs::theFormatButtonSendsTheStatementAndTabWidth()
{
    const QString sql = QStringLiteral("select id,name from users where id=7");
    const QString formatted = QStringLiteral("SELECT\n    id,\n    name\nFROM\n    users");
    m_backend.replyWithResult(formatted);

    QWidget parent;
    bool heldDown = true;
    QString shown;
    const bool opened = withModal(
        [&parent, &sql]() { showQueryDialog(&parent, QStringLiteral("query"), sql); },
        [&](QDialog &dlg)
        {
            auto *body = dlg.findChild<QPlainTextEdit *>();
            QPushButton *format = button(dlg, QStringLiteral("Format"));
            if (!body || !format)
            {
                return;
            }
            format->click();
            // Held down while the RPC is out, so the same statement cannot be
            // queued twice.
            heldDown = !format->isEnabled();
            waitUntil([body, &formatted] { return body->toPlainText() == formatted; });
            shown = body->toPlainText();
        }
    );

    QVERIFY(opened);
    QVERIFY(heldDown);
    QCOMPARE(shown, formatted);

    QCOMPARE(callsTo(m_backend, FormatPath).size(), 1);
    const StubBackend::Request req = callsTo(m_backend, FormatPath).at(0);
    QCOMPARE(req.verb, QStringLiteral("POST"));
    // The width is the dialog's, not the Tab Size preference: this is a
    // read-only view of somebody else's statement, not an editor.
    QCOMPARE(req.args, QJsonArray({sql, FormatTabWidth}));
}

void TestSmallDialogs::aFormatFailureIsShownAndChangesNothing()
{
    const QString sql = QStringLiteral("select from where");
    m_backend.replyWithError(QStringLiteral("syntax error near \"from\""));

    QWidget parent;
    QString message;
    QString kept;
    bool retryable = false;
    const bool opened = withModal(
        [&parent, &sql]() { showQueryDialog(&parent, QStringLiteral("query"), sql); },
        [&](QDialog &dlg)
        {
            auto *body = dlg.findChild<QPlainTextEdit *>();
            QPushButton *format = button(dlg, QStringLiteral("Format"));
            QLabel *error = smallText(dlg);
            if (!body || !format || !error)
            {
                return;
            }
            format->click();
            waitUntil([error] { return !error->isHidden(); });
            message = error->text();
            kept = body->toPlainText();
            retryable = format->isEnabled();
        }
    );

    QVERIFY(opened);
    QCOMPARE(message, QStringLiteral("format: syntax error near \"from\""));
    // Nothing came back to put there, so what the connection is running stays
    // on screen exactly as it was.
    QCOMPARE(kept, sql);
    QVERIFY(retryable);
}

void TestSmallDialogs::theCopyButtonTakesTheWholeStatement()
{
    const QString sql = QStringLiteral("SELECT *\nFROM orders");

    QWidget parent;
    QApplication::clipboard()->clear();
    const bool opened = withModal(
        [&parent, &sql]() { showQueryDialog(&parent, QStringLiteral("query"), sql); },
        [](QDialog &dlg)
        {
            if (QPushButton *copy = button(dlg, QStringLiteral("Copy")))
            {
                copy->click();
            }
        }
    );

    QVERIFY(opened);
    // The body, not the argument: after a Format the two differ, and what is
    // on screen is what the user meant to take.
    QCOMPARE(QApplication::clipboard()->text(), sql);
}

void TestSmallDialogs::theQueryDialogClosesOnItsCloseButton()
{
    QWidget parent;
    bool found = false;
    const bool opened = withModal(
        [&parent]()
        { showQueryDialog(&parent, QStringLiteral("query"), QStringLiteral("SELECT 1")); },
        [&found](QDialog &dlg)
        {
            QPushButton *close = roleButton(dlg, QDialogButtonBox::RejectRole);
            found = close != nullptr;
            if (close)
            {
                close->click();
            }
        }
    );

    QVERIFY(opened);
    QVERIFY(found);
    // The button ends the exec() by itself; nothing had to be closed for it.
    QVERIFY(!QApplication::activeModalWidget());
}

void TestSmallDialogs::theAboutBoxWaitsForTheBuildStamps()
{
    m_backend.replyWithResult(QJsonObject{{"version", "1.4.2"}});

    QWidget parent;
    bool upBeforeTheAnswer = true;
    const bool opened = withModal(
        [&parent, &upBeforeTheAnswer]()
        {
            showAboutDialog(&parent);
            upBeforeTheAnswer = QApplication::activeModalWidget() != nullptr;
        },
        [](QDialog &) {}
    );

    QVERIFY(opened);
    // An About box with an empty build line would be worse than a beat's wait,
    // so nothing is on screen until admin.AppInfo has answered.
    QVERIFY(!upBeforeTheAnswer);
    QCOMPARE(callsTo(m_backend, AppInfoPath).size(), 1);
    QCOMPARE(callsTo(m_backend, AppInfoPath).at(0).args, QJsonArray());
}

void TestSmallDialogs::theBuildLineJoinsVersionCommitAndDate()
{
    const QString iso = QStringLiteral("2026-01-02T03:04:05Z");
    m_backend.replyWithResult(
        QJsonObject{{"version", "1.4.2"}, {"commit", "9f3c1ab"}, {"date", iso}}
    );

    QWidget parent;
    QString title;
    QString heading;
    QString build;
    const bool opened = withModal(
        [&parent]() { showAboutDialog(&parent); },
        [&](QDialog &dlg)
        {
            title = dlg.windowTitle();
            if (QLabel *name = dlg.findChild<QLabel *>(QStringLiteral("kpiValue")))
            {
                heading = name->text();
            }
            if (QLabel *stamps = smallText(dlg))
            {
                build = stamps->text();
            }
        }
    );

    QVERIFY(opened);
    QCOMPARE(title, QStringLiteral("About mybench"));
    QCOMPARE(heading, QStringLiteral("mybench"));

    // The stamp is shown in the reader's own zone, so the expected string is
    // the same conversion rather than a pinned hour.
    const QString when =
        QDateTime::fromString(iso, Qt::ISODate).toLocalTime().toString("yyyy-MM-dd HH:mm");
    QCOMPARE(build, QStringLiteral("version 1.4.2 · build 9f3c1ab · %1").arg(when));
}

void TestSmallDialogs::aDevBuildWithNoStampsShowsNoBuildLine()
{
    // What a `go build` with no ldflags reports: the keys are there, empty.
    m_backend.replyWithResult(QJsonObject{{"version", ""}, {"commit", ""}, {"date", ""}});

    QWidget parent;
    QString build;
    bool visible = true;
    const bool opened = withModal(
        [&parent]() { showAboutDialog(&parent); },
        [&](QDialog &dlg)
        {
            QLabel *stamps = smallText(dlg);
            if (!stamps)
            {
                return;
            }
            build = stamps->text();
            visible = !stamps->isHidden();
        }
    );

    QVERIFY(opened);
    // An empty line reads as a missing one; the row goes instead of showing
    // "version · build".
    QVERIFY(build.isEmpty());
    QVERIFY(!visible);
}

void TestSmallDialogs::anUnparsableBuildDateIsShownAsItCame()
{
    m_backend.replyWithResult(QJsonObject{{"date", "last Tuesday"}});

    QWidget parent;
    QString build;
    const bool opened = withModal(
        [&parent]() { showAboutDialog(&parent); },
        [&](QDialog &dlg)
        {
            if (QLabel *stamps = smallText(dlg))
            {
                build = stamps->text();
            }
        }
    );

    QVERIFY(opened);
    // Not RFC3339, so it is passed through rather than dropped: whatever the
    // build stamped is still the only thing identifying it.
    QCOMPARE(build, QStringLiteral("last Tuesday"));
}

void TestSmallDialogs::anAppInfoFailureTakesTheBuildLinesPlace()
{
    m_backend.replyWithError(QStringLiteral("admin.AppInfo: connection refused"));

    QWidget parent;
    QString build;
    bool visible = false;
    int rows = 0;
    const bool opened = withModal(
        [&parent]() { showAboutDialog(&parent); },
        [&](QDialog &dlg)
        {
            rows = int(credits(dlg).size());
            QLabel *stamps = smallText(dlg);
            if (!stamps)
            {
                return;
            }
            build = stamps->text();
            visible = !stamps->isHidden();
        }
    );

    QVERIFY(opened);
    // The box still opens: the error goes where the stamps would have been,
    // and everything that does not depend on the backend is still listed.
    QCOMPARE(build, QStringLiteral("admin.AppInfo: connection refused"));
    QVERIFY(visible);
    QCOMPARE(rows, 5);
}

void TestSmallDialogs::theCreditsNameEveryProjectTheClientShips()
{
    m_backend.replyWithResult(QJsonObject{{"version", "1.4.2"}});

    QWidget parent;
    QVector<QPair<QString, QString>> rows;
    const bool opened = withModal(
        [&parent]() { showAboutDialog(&parent); }, [&rows](QDialog &dlg) { rows = credits(dlg); }
    );

    QVERIFY(opened);
    QCOMPARE(rows.size(), 5);

    // The licences do not require in-app attribution, which is exactly why a
    // silently dropped row would never be noticed anywhere else.
    const QVector<QPair<QString, QString>> want{
        {QStringLiteral("https://www.qt.io"), QStringLiteral("Qt 6")},
        {QStringLiteral("https://go.dev"), QStringLiteral("Go")},
        {QStringLiteral("https://github.com/go-sql-driver/mysql"),
         QStringLiteral("go-sql-driver/mysql")},
        {QStringLiteral("https://lucide.dev"), QStringLiteral("Lucide")},
        {QStringLiteral("https://www.nerdfonts.com"), QStringLiteral("RobotoMono Nerd Font")},
    };
    for (int i = 0; i < want.size(); ++i)
    {
        QVERIFY2(
            rows.at(i).first.contains(want.at(i).first),
            qPrintable(QStringLiteral("row %1 lost its link: %2").arg(i).arg(rows.at(i).first))
        );
        QVERIFY2(
            rows.at(i).first.contains(want.at(i).second),
            qPrintable(QStringLiteral("row %1 lost its name: %2").arg(i).arg(rows.at(i).first))
        );
        QVERIFY(!rows.at(i).second.isEmpty());
    }

    QCOMPARE(rows.at(0).second, QStringLiteral("GUI toolkit (LGPLv3)"));
    QCOMPARE(rows.at(1).second, QStringLiteral("Backend"));
    QCOMPARE(rows.at(2).second, QStringLiteral("pure-Go MySQL Driver"));
    QCOMPARE(rows.at(3).second, QStringLiteral("Icons (ISC)"));
    QCOMPARE(rows.at(4).second, QStringLiteral("Typeface"));
}

void TestSmallDialogs::everyShortcutRowHasKeysAndADescription()
{
    QWidget parent;
    QVector<ShortcutGroup> groups;
    const bool opened = withModal(
        [&parent]() { showShortcutsDialog(&parent); },
        [&groups](QDialog &dlg) { groups = shortcutGroups(dlg); }
    );

    QVERIFY(opened);
    QVERIFY(!groups.isEmpty());

    // A binding that loses its key text leaves a blank column, which reads as
    // "no shortcut" rather than as a broken reference.
    for (const ShortcutGroup &group : groups)
    {
        QVERIFY(!group.title.isEmpty());
        QVERIFY(!group.rows.isEmpty());
        for (const ShortcutRow &row : group.rows)
        {
            QVERIFY2(
                !row.keys.trimmed().isEmpty(),
                qPrintable(QStringLiteral("%1: no keys for %2").arg(group.title, row.what))
            );
            QVERIFY2(
                !row.what.trimmed().isEmpty(),
                qPrintable(QStringLiteral("%1: nothing said about %2").arg(group.title, row.keys))
            );
        }
    }
}

void TestSmallDialogs::noTwoShortcutsClaimTheSameKeys()
{
    QWidget parent;
    QVector<ShortcutGroup> groups;
    const bool opened = withModal(
        [&parent]() { showShortcutsDialog(&parent); },
        [&groups](QDialog &dlg) { groups = shortcutGroups(dlg); }
    );

    QVERIFY(opened);

    // The table is kept by hand next to the bindings it documents, so two rows
    // spelling the same keys means one of them is describing a binding that
    // has been taken over.
    QSet<QString> seen;
    int rows = 0;
    for (const ShortcutGroup &group : groups)
    {
        for (const ShortcutRow &row : group.rows)
        {
            ++rows;
            QVERIFY2(
                !seen.contains(row.keys),
                qPrintable(QStringLiteral("%1 is listed twice").arg(row.keys))
            );
            seen.insert(row.keys);
        }
    }
    QCOMPARE(int(seen.size()), rows);
}

void TestSmallDialogs::theShortcutsAreGroupedTheWayTheUiIs()
{
    QWidget parent;
    QVector<ShortcutGroup> groups;
    QString title;
    const bool opened = withModal(
        [&parent]() { showShortcutsDialog(&parent); },
        [&](QDialog &dlg)
        {
            title = dlg.windowTitle();
            groups = shortcutGroups(dlg);
        }
    );

    QVERIFY(opened);
    QCOMPARE(title, QStringLiteral("Keyboard Shortcuts"));

    QStringList titles;
    QList<int> counts;
    for (const ShortcutGroup &group : groups)
    {
        titles << group.title;
        counts << int(group.rows.size());
    }
    QCOMPARE(
        titles, QStringList(
                    {QStringLiteral("Application"), QStringLiteral("Query Tabs"),
                     QStringLiteral("Editor"), QStringLiteral("Results"), QStringLiteral("Mouse")}
                )
    );
    QCOMPARE(counts, QList<int>({3, 2, 4, 1, 5}));

    // The last group documents the pointer, so its left column is prose rather
    // than a key sequence.
    QStringList gestures;
    for (const ShortcutRow &row : groups.last().rows)
    {
        gestures << row.keys;
    }
    QCOMPARE(
        gestures,
        QStringList(
            {QStringLiteral("Middle-click a tab"), QStringLiteral("Double-click a query tab"),
             QStringLiteral("Right-click the tab bar"), QStringLiteral("Right-click a result cell"),
             QStringLiteral("Double-click a result cell")}
        )
    );
}

void TestSmallDialogs::theBindingsMatchTheOnesTheAppInstalls()
{
    QWidget parent;
    QVector<ShortcutGroup> groups;
    const bool opened = withModal(
        [&parent]() { showShortcutsDialog(&parent); },
        [&groups](QDialog &dlg) { groups = shortcutGroups(dlg); }
    );

    QVERIFY(opened);
    QCOMPARE(groups.size(), 5);

    // Rendered as the platform spells them, so the reference reads the same as
    // the menus do: Ctrl here, the glyphs on macOS. Quick Connect is compared
    // against the function the shortcut is actually installed from, so the two
    // cannot drift apart.
    QCOMPARE(groups.at(0).rows.at(0).keys, native(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T)));
    QCOMPARE(groups.at(0).rows.at(1).keys, native(quickConnectShortcut()));
    QCOMPARE(groups.at(0).rows.at(2).keys, native(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W)));
    QCOMPARE(groups.at(1).rows.at(0).keys, native(QKeySequence(Qt::CTRL | Qt::Key_T)));
    QCOMPARE(groups.at(1).rows.at(1).keys, native(QKeySequence(Qt::CTRL | Qt::Key_W)));
    QCOMPARE(groups.at(2).rows.at(0).keys, native(QKeySequence(Qt::CTRL | Qt::Key_Return)));
    QCOMPARE(
        groups.at(2).rows.at(1).keys, native(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Return))
    );
    QCOMPARE(groups.at(2).rows.at(2).keys, native(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F)));
    QCOMPARE(groups.at(2).rows.at(3).keys, native(QKeySequence(Qt::CTRL | Qt::Key_Space)));
    QCOMPARE(groups.at(3).rows.at(0).keys, native(QKeySequence(QKeySequence::Copy)));

    // Which binding each row documents, so a description cannot slide onto the
    // neighbouring keys.
    QCOMPARE(groups.at(0).rows.at(0).what, QStringLiteral("Open the Connections dialog"));
    QCOMPARE(groups.at(0).rows.at(1).what, QStringLiteral("Quick Connect to a Teleport database"));
    QCOMPARE(groups.at(1).rows.at(0).what, QStringLiteral("New query tab"));
    QCOMPARE(groups.at(2).rows.at(0).what, QStringLiteral("Run the statement at the cursor"));
    QCOMPARE(groups.at(2).rows.at(1).what, QStringLiteral("Run the whole script"));
    QCOMPARE(
        groups.at(3).rows.at(0).what,
        QStringLiteral("Copy the selected cells (Copy Separator preference)")
    );
}

void TestSmallDialogs::theShortcutsDialogTalksToNoBackend()
{
    QWidget parent;
    const bool opened = withModal([&parent]() { showShortcutsDialog(&parent); }, [](QDialog &) {});

    QVERIFY(opened);
    // The table is hard-coded next to the bindings, so it opens with no
    // backend at all: the Help menu still works while a connection is down.
    api()->flush(2000);
    QTest::qWait(50);
    QVERIFY(m_backend.requests().isEmpty());
}

QTEST_MAIN(TestSmallDialogs)

#include "tst_smalldialogs.moc"
