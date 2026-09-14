#include "editor/editortab.h"

#include "app/api.h"
#include "app/icons.h"
#include "app/theme.h"
#include "editor/planview.h"
#include "editor/resultgrid.h"
#include "editor/resultmodel.h"
#include "editor/resultpage.h"
#include "editor/sqleditor.h"
#include "editor/sqlscan.h"
#include "ui/handcursor.h"
#include "ui/widgets.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace
{

constexpr int PollMs = 250;
// The note is elided to this rather than given a flexible policy: squeezed to
// zero it silently swallowed messages, and allowed to grow it widened the
// window.
constexpr int NoteMaxPx = 340;

// A statement that already starts with EXPLAIN is unwrapped before we re-wrap
// it, so "Explain" on an EXPLAIN buffer does not double up.
const QRegularExpression &stripExplain()
{
    static const QRegularExpression re(
        R"(^\s*explain\s+(analyze\s+)?(format\s*=\s*\w+\s+)?)",
        QRegularExpression::CaseInsensitiveOption
    );
    return re;
}

} // namespace

EditorTab::EditorTab(
    const QString &connID, const QString &tabID, const QString &initialSQL, QWidget *parent
)
    : QWidget(parent), m_connID(connID), m_tabID(tabID)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // --- editor + toolbar (top half of the splitter) ----------------------
    auto *top = new QWidget;
    auto *topLayout = new QVBoxLayout(top);
    topLayout->setContentsMargins(12, 12, 12, 8);
    topLayout->setSpacing(8);

    m_editor = new SqlEditor;
    m_editor->document()->setPlainText(initialSQL);
    topLayout->addWidget(m_editor, 1);

    auto *bar = new QHBoxLayout;
    bar->setSpacing(8);

    // Left of the bar: what can be done to the buffer.
    m_runBtn = new QPushButton(tr("Run"));
    m_runBtn->setProperty("variant", "primary");
    m_runBtn->setToolTip(tr("Run every statement in the selection, or in the whole editor when "
                            "nothing is selected — one result tab each, in order"));
    m_runStmtBtn = new QPushButton(tr("Run Statement"));
    m_runStmtBtn->setToolTip(tr("Run only the statement under the cursor (Ctrl+Enter)"));
    m_explainBtn = new QPushButton(tr("Explain"));
    m_analyzeBtn = new QPushButton(tr("Explain Analyze"));
    m_analyzeBtn->setToolTip(tr("Runs EXPLAIN ANALYZE — the statement is actually executed"));
    m_formatBtn = new QPushButton(tr("Format"));
    m_formatBtn->setToolTip(tr("Format the selection or the whole buffer (Ctrl+Shift+F)"));
    m_snippetBtn = new QToolButton;
    m_snippetBtn->setObjectName("SnippetsBtn"); // button-look rules in theme.cpp
    m_snippetBtn->setText(tr("Snippets"));
    m_snippetBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_snippetBtn->setPopupMode(QToolButton::InstantPopup);
    m_snippetBtn->setMenu(new QMenu(m_snippetBtn));
    m_limitCombo = new QComboBox;
    // Workbench-style per-tab row limit. Fetching is what is limited — the
    // SQL is never modified, the server still runs the full query.
    for (int n : {10, 100, 1000, 10000})
    {
        m_limitCombo->addItem(tr("Limit %L1").arg(n), n);
    }
    m_limitCombo->addItem(tr("Use Global Limit"), 0);
    m_limitCombo->setCurrentIndex(m_limitCombo->count() - 1);
    m_limitCombo->setToolTip(tr("Rows a run fetches into memory before truncating the result"));
    m_cancelBtn = new QPushButton(tr("Cancel"));
    m_cancelBtn->setProperty("variant", "destructive");
    m_cancelBtn->setVisible(false);

    bar->addWidget(m_runBtn);
    bar->addWidget(m_runStmtBtn);
    bar->addWidget(m_explainBtn);
    bar->addWidget(m_analyzeBtn);
    bar->addWidget(m_formatBtn);
    bar->addWidget(m_snippetBtn);
    bar->addWidget(m_limitCombo);
    bar->addWidget(m_cancelBtn);

    // Right of the bar: what the current result offers. All of it is hidden
    // until there is one — the keyboard hint holds the space in the meantime,
    // and yields it (Ignored) rather than widening the tab.
    m_hintLbl = mutedLabel(tr("Ctrl+Enter: statement at cursor · Ctrl+Shift+Enter: whole script"));
    m_hintLbl->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_rowsLbl = mutedLabel();
    m_exportBtn = new QPushButton(tr("Export CSV"));
    m_exportBtn->setVisible(false);
    m_applyBtn = new QPushButton(tr("Apply Edits"));
    m_applyBtn->setVisible(false);
    m_discardBtn = new QPushButton(tr("Discard"));
    m_discardBtn->setProperty("variant", "ghost");
    m_discardBtn->setVisible(false);
    m_noteLbl = mutedLabel();
    m_noteLbl->setMaximumWidth(NoteMaxPx);

    bar->addWidget(m_hintLbl);
    bar->addWidget(m_rowsLbl);
    bar->addWidget(m_exportBtn);
    bar->addWidget(m_applyBtn);
    bar->addWidget(m_discardBtn);
    bar->addWidget(m_noteLbl);
    bar->addStretch();
    topLayout->addLayout(bar);

    // The failed-statement strip. Its frame is palette-dependent, so the
    // colours come from applyTheme() rather than from a sheet baked here.
    m_errorLbl = new QLabel;
    m_errorLbl->setWordWrap(true);
    m_errorLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_errorLbl->setVisible(false);
    topLayout->addWidget(m_errorLbl);
    applyTheme();

    // --- results (bottom half) -------------------------------------------
    m_resultTabs = new QTabWidget;
    // No document mode — it suppresses the stylesheet pane frame that draws
    // the separator under the tab strip (see theme.cpp).
    m_resultTabs->setTabsClosable(false);
    handCursorOnTabs(m_resultTabs->tabBar());
    m_plan = new PlanView;
    auto *empty = new QWidget;

    // `empty` goes in first so a fresh tab shows nothing below the editor;
    // the pages after it are switched to by widget, not by index.
    m_bottom = new QStackedWidget;
    m_bottom->addWidget(empty);
    m_bottom->addWidget(m_resultTabs);
    m_bottom->addWidget(m_plan);

    m_split = new QSplitter(Qt::Vertical);
    m_split->addWidget(top);
    m_split->addWidget(m_bottom);
    m_split->setStretchFactor(0, 0);
    m_split->setStretchFactor(1, 1);
    m_split->setChildrenCollapsible(false);
    root->addWidget(m_split, 1);

    // --- running ----------------------------------------------------------
    connect(m_runBtn, &QPushButton::clicked, this, [this]() { runScript(); });
    connect(m_runStmtBtn, &QPushButton::clicked, this, [this]() { runStatement(); });
    connect(m_editor, &SqlEditor::runRequested, this, [this]() { runStatement(); });
    connect(m_editor, &SqlEditor::runScriptRequested, this, [this]() { runScript(); });
    connect(m_explainBtn, &QPushButton::clicked, this, [this]() { explain(false); });
    connect(m_resultTabs, &QTabWidget::currentChanged, this, [this]() { updateToolbar(); });

    // --- the Explain Analyze arm ------------------------------------------
    connect(
        m_analyzeBtn, &QPushButton::clicked, this,
        [this]()
        {
            const QString stmt = m_editor->statementToRun().remove(stripExplain());
            static const QRegularExpression readOnly(
                R"(^\s*(select|with|table)\b)", QRegularExpression::CaseInsensitiveOption
            );
            // EXPLAIN ANALYZE executes the statement; non-read statements need a
            // second click inside the arm window.
            if (!readOnly.match(stmt).hasMatch() && !m_armedAnalyze)
            {
                m_armedAnalyze = true;
                m_analyzeBtn->setText(tr("Executes Statement — Confirm"));
                m_analyzeBtn->setProperty("variant", "destructive");
                m_analyzeBtn->style()->polish(m_analyzeBtn);
                applyButtonIcons();
                m_armTimer->start(4000);
                return;
            }
            m_armTimer->stop();
            m_armedAnalyze = false;
            m_analyzeBtn->setText(tr("Explain Analyze"));
            m_analyzeBtn->setProperty("variant", QVariant());
            m_analyzeBtn->style()->polish(m_analyzeBtn);
            applyButtonIcons();
            explain(true);
        }
    );
    m_armTimer = new QTimer(this);
    m_armTimer->setSingleShot(true);
    connect(
        m_armTimer, &QTimer::timeout, this,
        [this]()
        {
            m_armedAnalyze = false;
            m_analyzeBtn->setText(tr("Explain Analyze"));
            m_analyzeBtn->setProperty("variant", QVariant());
            m_analyzeBtn->style()->polish(m_analyzeBtn);
            applyButtonIcons();
        }
    );

    // --- formatting -------------------------------------------------------
    auto doFormat = [this]()
    {
        // sql-formatter has no C++ equivalent, so formatting lives in the Go
        // backend now (internal/sqlfmt) — one RPC, same keybinding.
        const bool whole = !m_editor->textCursor().hasSelection();
        const QString src =
            whole ? m_editor->toPlainText()
                  : m_editor->textCursor().selectedText().replace(QChar(0x2029), '\n');
        if (src.trimmed().isEmpty())
        {
            return;
        }
        api()->call(
            "sqlfmt", "Format", {src, 4}, this,
            [this, whole](const QJsonValue &res, const QString &err)
            {
                if (!err.isEmpty())
                {
                    setError("format: " + err);
                    return;
                }
                setError({});
                const QString pretty = res.toString();
                if (whole)
                {
                    QTextCursor c = m_editor->textCursor();
                    const int pos = c.position();
                    c.select(QTextCursor::Document);
                    c.insertText(pretty);
                    c.setPosition(int(qMin<qsizetype>(pos, pretty.size())));
                    m_editor->setTextCursor(c);
                }
                else
                {
                    m_editor->textCursor().insertText(pretty);
                }
            }
        );
    };
    connect(m_formatBtn, &QPushButton::clicked, this, doFormat);
    connect(m_editor, &SqlEditor::formatRequested, this, doFormat);

    // --- the result-side actions ------------------------------------------
    connect(
        m_cancelBtn, &QPushButton::clicked, this,
        [this]()
        {
            // Cancels the statement in flight; the rest of the queue is dropped.
            const QString id = m_planKind != PlanKind::None
                                   ? m_planResultId
                                   : (currentPage() ? currentPage()->resultId() : QString());
            if (!id.isEmpty())
            {
                api()->post("query", "Cancel", {id});
            }
            m_queue.clear();
            // The server's acknowledgement is not waited for, and post()'s own
            // failure is never reported, so the tab has to stand itself back up
            // here. Leaving m_running set strands Run behind a State poll that
            // may never come.
            queueFinished(RunOutcome::Cancelled);
        }
    );
    connect(m_exportBtn, &QPushButton::clicked, this, &EditorTab::exportCsv);
    connect(m_applyBtn, &QPushButton::clicked, this, &EditorTab::previewEdits);
    connect(
        m_discardBtn, &QPushButton::clicked, this,
        [this]()
        {
            if (ResultPage *p = currentPage())
            {
                p->model()->discardStaged();
            }
        }
    );
    connect(m_snippetBtn->menu(), &QMenu::aboutToShow, this, &EditorTab::loadSnippets);
    connect(m_editor, &SqlEditor::documentSettled, this, &EditorTab::sqlChanged);
    connect(m_editor, &SqlEditor::documentSettled, this, &EditorTab::requestLint);

    // Started and stopped around a run; nothing polls while the tab is idle.
    m_poll = new QTimer(this);
    m_poll->setInterval(PollMs);
    connect(m_poll, &QTimer::timeout, this, &EditorTab::pollState);

    // A reload never runs the unmount path, so reap anything a previous run
    // orphaned under this tabID. No session exists yet, so closing is free.
    api()->post("query", "CloseTab", {m_connID, m_tabID});
}

EditorTab::~EditorTab()
{
    clearPages();
    if (!m_planResultId.isEmpty())
    {
        api()->post("query", "CloseResult", {m_planResultId});
    }
    api()->post("query", "CloseTab", {m_connID, m_tabID});
}

QString EditorTab::sql() const
{
    return m_editor->toPlainText();
}

int EditorTab::editorHeight() const
{
    return m_split->sizes().value(0);
}

void EditorTab::setEditorHeight(int px)
{
    if (px <= 0)
    {
        return;
    }
    const int total = qMax(m_split->height(), px + 120);
    m_split->setSizes({px, total - px});
}

void EditorTab::setDefaultRowLimit(int rows)
{
    if (rows >= 0)
    {
        m_defaultRowLimit = rows;
    }
}

void EditorTab::applyButtonIcons()
{
    const AppPalette &pal = theme::current();
    // The UI font's pixel size (DPI-corrected), and an explicit iconSize:
    // without it the style scales the pixmap up to its 16px default, which
    // never matches the label text.
    const int px = theme::scaledPx(1.0);
    const auto set = [px](QAbstractButton *b, const char *name, const QColor &colour)
    {
        b->setIcon(icons::icon(name, colour, px));
        b->setIconSize(QSize(px, px));
    };
    set(m_runBtn, "play", pal.primaryFg);
    set(m_runStmtBtn, "square-play", pal.foreground);
    set(m_explainBtn, "route", pal.foreground);
    const bool armed = m_analyzeBtn->property("variant").toString() == "destructive";
    set(m_analyzeBtn, "timer", armed ? pal.background : pal.foreground);
    set(m_formatBtn, "wand-sparkles", pal.foreground);
    set(m_snippetBtn, "bookmark", pal.foreground);
    set(m_cancelBtn, "circle-stop", pal.background);
    set(m_exportBtn, "file-down", pal.foreground);
    set(m_applyBtn, "check", pal.foreground);
    set(m_discardBtn, "undo-2", pal.mutedFg);
}

void EditorTab::applyTheme()
{
    const AppPalette &pal = theme::current();
    m_errorLbl->setStyleSheet(
        QString("QLabel { color: %1; border: 1px solid %2; border-radius: 6px; padding: 6px; }")
            .arg(
                pal.destructive.name(),
                QColor(pal.destructive.red(), pal.destructive.green(), pal.destructive.blue(), 128)
                    .name(QColor::HexArgb)
            )
    );
    applyButtonIcons();
}

void EditorTab::applyEditorPrefs(const QString &editorThemeId, int fontSizePx, int tabChars)
{
    m_editor->setEditorPalette(theme::editor(editorThemeId));
    m_editor->setEditorFontSize(fontSizePx);
    m_editor->setTabWidthChars(tabChars);
}

void EditorTab::setConnected(bool on)
{
    m_connected = on;
    m_runBtn->setEnabled(on && !m_running);
    m_runStmtBtn->setEnabled(on && !m_running);
    if (on)
    {
        loadCompletionSchema();
    }
    // Re-lint either way: connecting gains the server check, disconnecting
    // drops stale server findings but keeps the placeholder warnings.
    requestLint(m_editor->toPlainText());
}

namespace
{

// Maps the server's "near '…' at line N" onto buffer offsets. The line is
// 1-based within the statement; the fragment is the input from the error
// point onwards, matched on a prefix because the server truncates the tail.
EditorDiagnostic
diagnosticFor(const StatementSpan &span, int line, const QString &near, const QString &message)
{
    int lineStart = 0;
    for (int l = 1; l < line && lineStart >= 0; ++l)
    {
        lineStart = int(span.text.indexOf('\n', lineStart));
        if (lineStart >= 0)
        {
            ++lineStart;
        }
    }
    lineStart = qMax(0, lineStart);
    int from = near.isEmpty() ? -1 : int(span.text.indexOf(near.left(24), lineStart));
    if (from < 0)
    {
        from = lineStart;
    }
    // Underline to the end of the offending line — enough to see, not the
    // whole rest of the statement.
    qsizetype to = span.text.indexOf('\n', from);
    if (to < 0)
    {
        to = span.text.size();
    }
    if (to <= from)
    {
        // The error sits at the very end ("near ''"); mark the last char.
        from = qMax(0, int(span.text.size()) - 1);
        to = span.text.size();
    }
    return {span.start + from, span.start + int(to), message};
}

} // namespace

void EditorTab::requestLint(const QString &sql)
{
    // Placeholders are client-side findings: they PREPARE cleanly, so the
    // server-side lint passes them, but plain execution fails on them — and
    // pasted-from-code queries carry them all the time. Warn even offline.
    QVector<EditorDiagnostic> local;
    for (int at : placeholderOffsets(sql))
    {
        local.append(
            {at, at + 1, tr("parameter placeholder — replace with a value before running"), true}
        );
    }
    const QVector<StatementSpan> spans = statementSpans(sql);
    if (!m_connected || spans.isEmpty())
    {
        m_editor->setDiagnostics(local);
        return;
    }
    QJsonArray stmts;
    for (const StatementSpan &span : spans)
    {
        stmts.append(span.text);
    }
    const int seq = ++m_lintSeq;
    api()->call(
        "query", "Lint", {m_connID, stmts}, this,
        [this, seq, spans, local](const QJsonValue &res, const QString &err)
        {
            if (seq != m_lintSeq || !err.isEmpty())
            {
                return; // superseded by newer edits, or backend unreachable
            }
            QVector<EditorDiagnostic> diags = local;
            for (const auto &v : res.toArray())
            {
                const QJsonObject o = v.toObject();
                const int at = o.value("statement").toInt();
                if (at < 0 || at >= spans.size())
                {
                    continue;
                }
                diags.append(diagnosticFor(
                    spans.at(at), o.value("line").toInt(), o.value("near").toString(),
                    o.value("message").toString()
                ));
            }
            m_editor->setDiagnostics(diags);
        }
    );
}

void EditorTab::loadCompletionSchema()
{
    api()->call(
        "admin", "SchemaMap", {m_connID}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                return; // completion is a nicety; never surface this
            }
            QHash<QString, QStringList> map;
            const QJsonObject o = res.toObject();
            for (auto it = o.constBegin(); it != o.constEnd(); ++it)
            {
                QStringList cols;
                for (const auto &c : it.value().toArray())
                {
                    if (!c.isNull())
                    {
                        cols << c.toString();
                    }
                }
                map.insert(it.key(), cols);
            }
            m_editor->setCompletionSchema(map);
        }
    );
}

void EditorTab::setNote(const QString &text, const char *tone)
{
    m_noteLbl->setProperty("tone", tone ? QVariant(QString::fromLatin1(tone)) : QVariant());
    m_noteLbl->style()->polish(m_noteLbl);
    m_noteLbl->setToolTip(text);
    m_noteLbl->setText(m_noteLbl->fontMetrics().elidedText(text, Qt::ElideMiddle, NoteMaxPx - 8));
}

void EditorTab::setError(const QString &message)
{
    m_errorLbl->setText(message);
    m_errorLbl->setVisible(!message.isEmpty());
}

ResultPage *EditorTab::currentPage() const
{
    return qobject_cast<ResultPage *>(m_resultTabs->currentWidget());
}

void EditorTab::clearPages()
{
    while (m_resultTabs->count() > 0)
    {
        QWidget *w = m_resultTabs->widget(0);
        m_resultTabs->removeTab(0);
        w->deleteLater(); // ResultPage releases its backend buffer in its dtor
    }
}

// ----------------------------------------------------------- execution ---

void EditorTab::runScript()
{
    const QTextCursor c = m_editor->textCursor();
    const QString source =
        c.hasSelection() ? c.selectedText().replace(QChar(0x2029), '\n') : m_editor->toPlainText();
    startQueue(splitStatements(source));
}

void EditorTab::runStatement()
{
    const QString stmt = m_editor->statementToRun();
    if (stmt.isEmpty())
    {
        return;
    }
    startQueue({stmt});
}

void EditorTab::startQueue(const QStringList &statements)
{
    if (statements.isEmpty() || m_running || !m_connected)
    {
        return;
    }
    setError({});
    m_noteLbl->clear();
    m_planKind = PlanKind::None;
    clearPages();

    m_queue = statements;
    m_queueTotal = int(statements.size());
    m_queueIndex = 0;
    m_running = true;
    m_runBtn->setEnabled(false);
    m_runStmtBtn->setEnabled(false);
    m_cancelBtn->setVisible(true);
    m_bottom->setCurrentWidget(m_resultTabs);
    runNext();
}

void EditorTab::runNext()
{
    if (m_queueIndex >= m_queue.size())
    {
        queueFinished(RunOutcome::Completed);
        return;
    }
    const QString stmt = m_queue.at(m_queueIndex);
    auto *page = new ResultPage(stmt);
    connect(
        page, &ResultPage::sortRequested, this, [this, page](int column) { sortBy(page, column); }
    );
    connect(page, &ResultPage::errorRaised, this, &EditorTab::setError);
    // A refused copy is not an error on the statement, so it goes to the note
    // line rather than the error strip the result itself owns.
    connect(
        page, &ResultPage::copyRefused, this,
        [this](const QString &reason) { setNote(reason, "warning"); }
    );
    connect(
        page, &ResultPage::stagedChanged, this,
        [this, page](int)
        {
            if (page == currentPage())
            {
                updateToolbar();
            }
        }
    );
    connect(
        page, &ResultPage::stateChanged, this,
        [this, page]()
        {
            if (page == currentPage())
            {
                updateToolbar();
            }
        }
    );

    // Numbered to match the script; the statement itself is the tooltip.
    const int index = m_resultTabs->addTab(page, tr("Result %1").arg(m_queueIndex + 1));
    m_resultTabs->setTabToolTip(index, stmt);
    m_resultTabs->setCurrentIndex(index);

    // The tab's limit dropdown, or the Default Row Limit preference while it
    // sits on "Use Global Limit"; -1 tells the backend to fetch without a cap
    // (a global limit of 0 means unlimited).
    const int tabLimit = m_limitCombo->currentData().toInt();
    const int effectiveMax = tabLimit > 0            ? tabLimit
                             : m_defaultRowLimit > 0 ? m_defaultRowLimit
                                                     : -1;
    api()->call(
        "query", "Run", {m_connID, m_tabID, stmt, effectiveMax}, this,
        [this, page](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                setError(err);
                queueFinished(RunOutcome::StoppedOnError);
                return;
            }
            const QJsonObject o = res.toObject();
            if (o.value("sessionReset").toBool())
            {
                setNote(tr("session was reset — SET/USE/transaction state is gone"), "warning");
            }
            page->applyState(o.value("state").toObject());
            if (page->done())
            {
                pollState(); // finished already: advance without waiting a tick
            }
            else
            {
                m_poll->start();
            }
        }
    );
}

void EditorTab::queueFinished(RunOutcome outcome)
{
    m_poll->stop();
    // Editability probes hit the same session, so they wait until no statement
    // is in flight.
    for (int i = 0; i < m_resultTabs->count(); ++i)
    {
        if (auto *p = qobject_cast<ResultPage *>(m_resultTabs->widget(i)))
        {
            p->resolveEditability();
        }
    }
    m_running = false;
    m_runBtn->setEnabled(m_connected);
    m_runStmtBtn->setEnabled(m_connected);
    m_cancelBtn->setVisible(false);
    if (outcome == RunOutcome::Cancelled)
    {
        setNote(tr("cancelled"), "warning");
    }
    else if (outcome == RunOutcome::Completed && m_queueTotal > 0)
    {
        setNote(
            m_queueTotal == 1 ? tr("ran 1 statement") : tr("ran %1 statements").arg(m_queueTotal)
        );
    }
    else if (outcome == RunOutcome::StoppedOnError && m_queueTotal > 1)
    {
        // Stopping is the safe default: later statements in a script usually
        // assume the earlier ones succeeded. A single statement needs no such
        // note — its result tab already tells the story.
        setNote(
            tr("stopped at statement %1 of %2").arg(m_queueIndex + 1).arg(m_queueTotal), "warning"
        );
    }
    m_queue.clear();
    updateToolbar();
}

void EditorTab::pollState()
{
    if (m_planKind != PlanKind::None)
    {
        const QString id = m_planResultId;
        if (id.isEmpty())
        {
            return;
        }
        api()->call(
            "query", "State", {id}, this,
            [this, id](const QJsonValue &res, const QString &err)
            {
                if (id != m_planResultId)
                {
                    return;
                }
                if (!err.isEmpty())
                {
                    m_poll->stop();
                    m_running = false;
                    m_runBtn->setEnabled(m_connected);
                    m_runStmtBtn->setEnabled(m_connected);
                    m_cancelBtn->setVisible(false);
                    setError(tr("lost result: %1").arg(err));
                    return;
                }
                const QJsonObject o = res.toObject();
                if (!o.value("done").toBool())
                {
                    return;
                }
                m_poll->stop();
                m_running = false;
                m_runBtn->setEnabled(m_connected);
                m_runStmtBtn->setEnabled(m_connected);
                m_cancelBtn->setVisible(false);
                const QString e = o.value("error").toString();
                if (!e.isEmpty())
                {
                    setError(e);
                    return;
                }
                fetchPlanText();
            }
        );
        return;
    }

    auto *page = qobject_cast<ResultPage *>(m_resultTabs->widget(m_queueIndex));
    if (!page || page->resultId().isEmpty())
    {
        return;
    }
    const QString id = page->resultId();
    api()->call(
        "query", "State", {id}, this,
        [this, page, id](const QJsonValue &res, const QString &err)
        {
            if (page->resultId() != id)
            {
                return;
            }
            if (!err.isEmpty())
            {
                // A dead poll must not leave the tab stuck "running" forever —
                // that silently blocks every future run.
                setError(tr("lost result: %1").arg(err));
                queueFinished(RunOutcome::StoppedOnError);
                return;
            }
            page->applyState(res.toObject());
            if (!page->done())
            {
                return;
            }
            m_poll->stop();
            if (!page->error().isEmpty())
            {
                // The result tab already shows the error in its summary —
                // repeating it in the strip under the buttons said it twice.
                queueFinished(RunOutcome::StoppedOnError);
                return;
            }
            emit statusMessage(tr("%L1 rows").arg(page->rowCount()));
            ++m_queueIndex;
            runNext();
        }
    );
}

void EditorTab::explain(bool analyze)
{
    QString stmt = m_editor->statementToRun().remove(stripExplain());
    if (stmt.trimmed().isEmpty() || m_running || !m_connected)
    {
        return;
    }
    setError({});
    m_noteLbl->clear();
    m_planKind = analyze ? PlanKind::Analyze : PlanKind::Explain;
    const QString sql = analyze ? "EXPLAIN ANALYZE " + stmt : "EXPLAIN FORMAT=TREE " + stmt;

    if (!m_planResultId.isEmpty())
    {
        api()->post("query", "CloseResult", {m_planResultId});
        m_planResultId.clear();
    }
    m_running = true;
    m_runBtn->setEnabled(false);
    m_runStmtBtn->setEnabled(false);
    m_cancelBtn->setVisible(true);
    api()->call(
        "query", "Run", {m_connID, m_tabID, sql, 0}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                setError(err);
                m_running = false;
                m_runBtn->setEnabled(m_connected);
                m_runStmtBtn->setEnabled(m_connected);
                m_cancelBtn->setVisible(false);
                return;
            }
            const QJsonObject state = res.toObject().value("state").toObject();
            m_planResultId = state.value("resultId").toString();
            m_bottom->setCurrentWidget(m_plan);
            if (state.value("done").toBool())
            {
                pollState();
            }
            else
            {
                m_poll->start();
            }
        }
    );
}

void EditorTab::fetchPlanText()
{
    const QString id = m_planResultId;
    api()->call(
        "query", "Rows", {id, 0, 4096}, this,
        [this, id](const QJsonValue &res, const QString &err)
        {
            if (id != m_planResultId)
            {
                return;
            }
            if (!err.isEmpty())
            {
                setError(err);
                return;
            }
            QStringList lines;
            for (const auto &r : res.toObject().value("rows").toArray())
            {
                QStringList cells;
                for (const auto &c : r.toArray())
                {
                    if (!c.isNull())
                    {
                        cells << c.toString();
                    }
                }
                lines << cells.join('\t');
            }
            m_plan->setPlan(lines.join('\n'));
            m_bottom->setCurrentWidget(m_plan);
        }
    );
}

// ------------------------------------------------------------- toolbar ---

void EditorTab::updateToolbar()
{
    ResultPage *page = currentPage();
    if (!page)
    {
        m_rowsLbl->clear();
        m_exportBtn->setVisible(false);
        m_applyBtn->setVisible(false);
        m_discardBtn->setVisible(false);
        return;
    }
    const int staged = int(page->model()->staged().size());
    QString rows;
    if (page->filtered())
    {
        rows = tr("%L1 of %L2 rows").arg(page->rowCount()).arg(page->totalRows());
    }
    else if (page->capped())
    {
        rows = tr("capped at %L1").arg(page->rowCount());
    }
    else
    {
        rows = tr("%L1 rows%2").arg(page->rowCount()).arg(page->done() ? "" : "…");
    }
    m_rowsLbl->setText(rows);
    m_exportBtn->setVisible(page->done() && page->rowCount() > 0 && page->error().isEmpty());
    m_applyBtn->setVisible(staged > 0);
    m_applyBtn->setText(staged == 1 ? tr("Apply 1 Edit") : tr("Apply %1 Edits").arg(staged));
    m_discardBtn->setVisible(staged > 0);
    if (staged == 0 && page->editable() && m_noteLbl->text().isEmpty())
    {
        setNote(tr("double-click a cell to edit"));
    }
}

void EditorTab::sortBy(ResultPage *page, int column)
{
    if (!page || !page->done() || m_running)
    {
        return;
    }
    if (!page->model()->staged().isEmpty())
    {
        setError(tr("apply or discard the staged edits before sorting"));
        return;
    }
    QHeaderView *header = page->grid()->horizontalHeader();
    const bool desc = header->sortIndicatorSection() == column &&
                      header->sortIndicatorOrder() == Qt::AscendingOrder;
    const QString id = page->resultId();
    // page is the context: a new run deletes the page mid-flight, and the
    // reply must be dropped with it rather than dereference the dead pointer.
    api()->call(
        "query", "Sort", {id, column, desc}, page,
        [this, page, id, column, desc](const QJsonValue &, const QString &err)
        {
            if (page->resultId() != id)
            {
                return;
            }
            if (!err.isEmpty())
            {
                setError(err);
                return;
            }
            QHeaderView *h = page->grid()->horizontalHeader();
            h->setSortIndicatorShown(true);
            h->setSortIndicator(column, desc ? Qt::DescendingOrder : Qt::AscendingOrder);
            page->model()->invalidateWindows();
        }
    );
}

void EditorTab::exportCsv()
{
    ResultPage *page = currentPage();
    if (!page)
    {
        return;
    }
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export CSV"), dir + "/mybench-" + page->resultId() + ".csv",
        tr("CSV Files (*.csv)")
    );
    if (path.isEmpty())
    {
        return;
    }
    api()->call(
        "query", "ExportCSV", {page->resultId(), path}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                setError(err);
                return;
            }
            setNote("→ " + res.toString());
        }
    );
}

// --------------------------------------------------------------- edits ---

QJsonObject EditorTab::stagedEditsPayload(ResultPage *page) const
{
    QJsonArray edits;
    for (const StagedEdit &e : page->model()->staged())
    {
        QJsonObject key;
        for (auto it = e.key.constBegin(); it != e.key.constEnd(); ++it)
        {
            key.insert(
                it.key(), it.value().isValid() ? QJsonValue(it.value().toString()) : QJsonValue()
            );
        }
        QJsonObject o;
        o.insert("key", key);
        o.insert("col", e.colName);
        o.insert("value", e.value.isValid() ? QJsonValue(e.value.toString()) : QJsonValue());
        edits.append(o);
    }
    QJsonObject wrapper;
    wrapper.insert("edits", edits);
    return wrapper;
}

void EditorTab::previewEdits()
{
    ResultPage *page = currentPage();
    if (!page)
    {
        return;
    }
    const QJsonArray edits = stagedEditsPayload(page).value("edits").toArray();
    // page as context: see sortBy.
    api()->call(
        "query", "PreviewEdits", {page->resultId(), edits}, page,
        [this, page](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                setError(err);
                return;
            }
            QStringList stmts;
            for (const auto &v : res.toArray())
            {
                if (!v.isNull())
                {
                    stmts << v.toString();
                }
            }

            QDialog dlg(this);
            dlg.setWindowTitle(tr("Apply Edits"));
            auto *lay = new QVBoxLayout(&dlg);
            lay->addWidget(
                mutedLabel(tr("These statements run in one transaction on this tab's session:"))
            );
            auto *body = new QPlainTextEdit(stmts.join('\n'));
            body->setReadOnly(true);
            body->setMinimumSize(620, 260);
            lay->addWidget(body);
            auto *box = new QDialogButtonBox(QDialogButtonBox::Cancel);
            auto *runBtn = box->addButton(tr("Run Updates"), QDialogButtonBox::AcceptRole);
            runBtn->setProperty("variant", "primary");
            connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
            connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
            lay->addWidget(box);
            if (dlg.exec() == QDialog::Accepted && page == currentPage())
            {
                applyEdits();
            }
        }
    );
}

void EditorTab::applyEdits()
{
    ResultPage *page = currentPage();
    if (!page)
    {
        return;
    }
    const QJsonArray edits = stagedEditsPayload(page).value("edits").toArray();
    // page as context: see sortBy.
    api()->call(
        "query", "ApplyEdits", {page->resultId(), edits}, page,
        [this, page](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                setError(err);
                return;
            }
            const int n = res.toInt();
            page->model()->discardStaged();
            page->model()->invalidateWindows(); // refetch from the patched buffer
            setNote(n == 1 ? tr("1 row updated") : tr("%1 rows updated").arg(n), "success");
            updateToolbar();
        }
    );
}

// ------------------------------------------------------------ snippets ---

void EditorTab::loadSnippets()
{
    QMenu *menu = m_snippetBtn->menu();
    menu->clear();
    menu->addAction(tr("Save Current Statement…"), this, &EditorTab::saveSnippetDialog);
    api()->call(
        "query", "Snippets", {}, this,
        [this, menu](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                return;
            }
            const QJsonArray arr = res.toArray();
            if (!arr.isEmpty())
            {
                menu->addSeparator();
            }
            // Deletion lives in its own submenu: setMenu() on the snippet action
            // itself would turn it into a submenu entry and make the insert
            // (triggered) unreachable by click.
            menu->setToolTipsVisible(true);
            QMenu *del = nullptr;
            for (const auto &v : arr)
            {
                const QJsonObject o = v.toObject();
                const QString name = o.value("name").toString();
                const QString sqlText = o.value("sql").toString();
                const qint64 id = qint64(o.value("id").toDouble());
                QAction *a = menu->addAction(name);
                a->setToolTip(sqlText);
                connect(
                    a, &QAction::triggered, this,
                    [this, sqlText]() { m_editor->insertSnippet(sqlText); }
                );
                if (!del)
                {
                    del = new QMenu(tr("Delete Snippet"), menu);
                }
                del->addAction(
                    name, this,
                    [this, id]()
                    {
                        api()->call(
                            "query", "DeleteSnippet", {id}, this,
                            [this](const QJsonValue &, const QString &e)
                            {
                                if (!e.isEmpty())
                                {
                                    setError(e);
                                }
                            }
                        );
                    }
                );
            }
            if (del)
            {
                menu->addSeparator();
                menu->addMenu(del);
            }
        }
    );
}

void EditorTab::saveSnippetDialog()
{
    const QString sqlText = m_editor->statementToRun();
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Save Snippet"));
    auto *lay = new QVBoxLayout(&dlg);
    auto *name = new QLineEdit;
    name->setPlaceholderText(tr("Snippet Name"));
    lay->addWidget(name);
    auto *preview = new QPlainTextEdit(
        sqlText.isEmpty() ? tr("(empty — put the cursor on a statement first)") : sqlText
    );
    preview->setReadOnly(true);
    preview->setMinimumSize(520, 160);
    lay->addWidget(preview);
    auto *box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    lay->addWidget(box);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    if (dlg.exec() != QDialog::Accepted || name->text().trimmed().isEmpty() || sqlText.isEmpty())
    {
        return;
    }
    api()->call(
        "query", "SaveSnippet", {name->text().trimmed(), sqlText}, this,
        [this](const QJsonValue &, const QString &err)
        {
            if (!err.isEmpty())
            {
                setError(err);
            }
        }
    );
}
