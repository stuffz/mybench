#pragma once
// One query tab: its own editor, its own backend session (connID+tabID), and a
// result tab per statement. Running a script runs its statements in order on
// that one session — so USE and SET carry — and each gets its own result tab,
// numbered to match the script.
#include <QJsonObject>
#include <QStringList>
#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTabWidget;
class QTimer;
class QToolButton;
class PlanView;
class ResultPage;
class SqlEditor;

class EditorTab : public QWidget
{
    Q_OBJECT
public:
    EditorTab(
        const QString &connID, const QString &tabID, const QString &initialSQL,
        QWidget *parent = nullptr
    );
    ~EditorTab() override;

    QString sql() const;
    void setConnected(bool on);
    int editorHeight() const;
    void setEditorHeight(int px);
    void applyEditorPrefs(const QString &editorThemeId, int fontSizePx, int tabChars);
    void applyTheme();
    // (Re)paints the toolbar button icons in the current palette; also run
    // when a button's variant flips (the armed Explain Analyze).
    void applyButtonIcons();
    // The Default Row Limit preference (0 = unlimited); the tab's limit
    // dropdown can pick a smaller per-tab value instead.
    void setDefaultRowLimit(int rows);

signals:
    void sqlChanged(const QString &sql);
    void statusMessage(const QString &message);

private:
    enum class PlanKind
    {
        None,
        Explain,
        Analyze
    };

    // Run every statement in the selection, or in the whole buffer when there
    // is none — one result tab each, in order.
    void runScript();
    // Run only the statement under the cursor (or the selection), one tab.
    void runStatement();
    void startQueue(const QStringList &statements);
    void runNext();
    // Why a run ended. Cancel is not an error and did not complete, and the
    // note line has to say something different for each.
    enum class RunOutcome
    {
        Completed,
        StoppedOnError,
        Cancelled,
    };
    void queueFinished(RunOutcome outcome);

    void explain(bool analyze);
    void pollState();
    void fetchPlanText();
    void clearPages();
    ResultPage *currentPage() const;
    void updateToolbar();

    void loadSnippets();
    void saveSnippetDialog();
    void previewEdits();
    void applyEdits();
    void exportCsv();
    void sortBy(ResultPage *page, int column);
    void setError(const QString &message);
    void setNote(const QString &text, const char *tone = nullptr);
    void loadCompletionSchema();
    // Live syntax check: PREPAREs the buffer's statements on the backend and
    // squiggles what the server rejects. Runs on the settle debounce.
    void requestLint(const QString &sql);
    QJsonObject stagedEditsPayload(ResultPage *page) const;

    QString m_connID, m_tabID;
    bool m_connected = false;
    int m_defaultRowLimit = 50000;

    int m_lintSeq = 0; // drops lint replies that newer edits superseded

    // Script execution: statements in order on the tab's one session.
    QStringList m_queue;
    int m_queueIndex = 0;
    int m_queueTotal = 0;
    bool m_running = false;

    // EXPLAIN goes through the same Run pipeline but renders as a tree, so it
    // keeps its own result id rather than a tab.
    PlanKind m_planKind = PlanKind::None;
    QString m_planResultId;
    bool m_armedAnalyze = false;

    SqlEditor *m_editor;
    QTabWidget *m_resultTabs;
    PlanView *m_plan;
    QStackedWidget *m_bottom;
    QSplitter *m_split;

    QPushButton *m_runBtn, *m_runStmtBtn, *m_explainBtn, *m_analyzeBtn, *m_formatBtn, *m_cancelBtn;
    QToolButton *m_snippetBtn;
    QComboBox *m_limitCombo;
    QPushButton *m_exportBtn, *m_applyBtn, *m_discardBtn;
    QLabel *m_rowsLbl, *m_hintLbl, *m_noteLbl, *m_errorLbl;
    QTimer *m_poll, *m_armTimer;
};
