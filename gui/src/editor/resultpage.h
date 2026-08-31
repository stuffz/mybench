#pragma once
// One result tab: the grid for a statement that returned rows, or a one-line
// summary for one that did not (INSERT, DDL, USE). Everything that belongs to
// a single result lives here — its buffer id, its model, its editability — so
// running a script of N statements is N of these rather than N special cases
// in the editor tab.
#include <QJsonObject>
#include <QWidget>

class QLabel;
class QLineEdit;
class QStackedWidget;
class QTimer;
class ResultGrid;
class ResultModel;

class ResultPage : public QWidget
{
    Q_OBJECT
public:
    explicit ResultPage(const QString &sql, QWidget *parent = nullptr);
    ~ResultPage() override;

    // Folds a ResultState into the page: columns, row count, done/capped, error.
    void applyState(const QJsonObject &state);
    // Resolves whether this result maps back to one table, enabling cell edits.
    void resolveEditability();

    QString sql() const { return m_sql; }

    QString resultId() const { return m_resultId; }

    ResultModel *model() const { return m_model; }

    ResultGrid *grid() const { return m_grid; }

    bool done() const { return m_done; }

    bool capped() const { return m_capped; }

    int rowCount() const { return m_rowCount; }

    int totalRows() const { return m_totalRows; }

    // A fuzzy row filter is active — rowCount() is the filtered count.
    bool filtered() const { return !m_filter.isEmpty(); }

    QString error() const { return m_error; }

    bool editable() const { return m_editable; }

    QString insertSchema() const { return m_insertSchema; }

    QString insertTable() const { return m_insertTable; }

    // Releases the backend buffer; called before the page is discarded.
    void closeResult();

signals:
    void stateChanged();
    void sortRequested(int column);
    void stagedChanged(int count);
    void errorRaised(const QString &message);

private:
    // The Ctrl+F fuzzy row filter over this result's backend buffer.
    void showFilterBar();
    void hideFilterBar();
    void requestFilter();

    QString m_sql;
    QString m_resultId;
    bool m_done = false;
    bool m_capped = false;
    int m_rowCount = 0;
    int m_totalRows = 0;
    QString m_error;
    bool m_editable = false;
    QString m_insertSchema, m_insertTable;
    QString m_filter; // last needle the backend accepted

    ResultModel *m_model;
    ResultGrid *m_grid;
    QLabel *m_summary;
    QStackedWidget *m_stack;
    QWidget *m_filterBar;
    QLineEdit *m_filterEdit;
    QTimer *m_filterDebounce;
};
