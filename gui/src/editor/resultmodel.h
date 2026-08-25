#pragma once
// Windowed view onto a Go-side result buffer. The model holds only the
// windows the viewport can see (SPEC.md: result data never enters the UI
// wholesale) — the same contract hooks/useResultWindow.ts had, expressed as
// a QAbstractTableModel so QTableView does the virtualisation for us.
#include <QAbstractTableModel>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVariant>
#include <QVector>

struct ColumnMeta
{
    QString name;
    QString type;
};

// One staged (not yet applied) cell change; identity captured from the key
// columns' current values so the backend can build the WHERE clause.
struct StagedEdit
{
    int row = 0;
    int col = 0;
    QString colName;
    QHash<QString, QVariant> key; // column name → value (invalid = NULL)
    QVariant value;               // invalid = NULL
};

class ResultModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    static constexpr int Window = 200;

    explicit ResultModel(QObject *parent = nullptr);

    // A new result, or the same result reordered server-side (sort) — either
    // way every cached window is stale.
    void setResult(const QString &resultId, const QVector<ColumnMeta> &cols, int rowCount);
    void setRowCount(int rowCount); // streaming growth
    void invalidateWindows();       // after a sort or an applied edit
    void clearResult();

    QString resultId() const { return m_resultId; }

    const QVector<ColumnMeta> &columns() const { return m_cols; }

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &ix, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &ix) const override;
    bool setData(const QModelIndex &ix, const QVariant &value, int role) override;

    // Raw cell access for copy actions and width fitting: invalid = NULL,
    // and a window still in flight also yields invalid.
    QVariant cell(int row, int col) const;
    bool rowLoaded(int row) const;

    // Editing
    void setEditable(
        bool on, const QSet<int> &editableCols, const QVector<QPair<QString, int>> &keyCols
    );

    bool editable() const { return m_editable; }

    const QHash<QString, StagedEdit> &staged() const { return m_staged; }

    void stage(const StagedEdit &e);
    void discardStaged();
    // "NULL" is not typeable in a line edit; the grid stages it explicitly.
    void stageNull(int row, int col);

signals:
    void windowArrived();
    void stagedChanged(int count);
    void error(const QString &message);

private:
    void fetchWindow(int windowIndex) const;

    static QString key(int row, int col)
    {
        return QString::number(row) + ":" + QString::number(col);
    }

    StagedEdit buildEdit(int row, int col, const QVariant &value) const;

    QString m_resultId;
    QVector<ColumnMeta> m_cols;
    int m_rows = 0;

    // mutable: data() is const but drives the lazy fetch.
    mutable QHash<int, QVector<QVector<QVariant>>> m_cache;
    mutable QSet<int> m_pending;

    bool m_editable = false;
    QSet<int> m_editableCols;
    QVector<QPair<QString, int>> m_keyCols;
    QHash<QString, StagedEdit> m_staged;
};
