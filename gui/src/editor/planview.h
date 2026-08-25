#pragma once
// EXPLAIN FORMAT=TREE / EXPLAIN ANALYZE rendered as a collapsible tree
// (SPEC.md: tree view, not graphics). Port of lib/explainTree.ts +
// components/PlanView.tsx; anything not TREE-shaped falls back to raw text.
#include <QVector>
#include <QWidget>
#include <optional>

class QTreeWidget;
class QTreeWidgetItem;
class QPlainTextEdit;
class QStackedWidget;

struct PlanNode
{
    QString name;
    std::optional<double> estCost, estRows;
    std::optional<double> actualFirstMs, actualLastMs, actualRows, loops;
    bool neverExecuted = false;
    // est vs actual rows off by ≥100× — the classic "optimizer had no idea".
    bool misestimate = false;
    QVector<PlanNode> children;
};

QVector<PlanNode> parsePlanTree(const QString &text);
QString fmtNum(double n);
QString fmtMs(double ms);

class PlanView : public QWidget
{
    Q_OBJECT
public:
    explicit PlanView(QWidget *parent = nullptr);
    void setPlan(const QString &text);

private:
    void addNode(const PlanNode &n, QTreeWidgetItem *parent);

    QTreeWidget *m_tree;
    QPlainTextEdit *m_raw;
    QStackedWidget *m_stack;
    QString m_text;
};
