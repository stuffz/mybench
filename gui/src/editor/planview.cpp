#include "editor/planview.h"

#include "app/theme.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <utility>

namespace
{

// MySQL prints sub-ms times in scientific notation ("599e-6"), so the number
// pattern must accept exponents, negative ones included. Function-local
// static: a file-scope QString would run a dynamic initialiser before main
// (cert-err58).
const QString &numPat()
{
    // The fraction is spelled out rather than [\d.]+ so the pattern cannot
    // run across the ".." of a range: greedy, it swallowed "1.05..3.25"
    // whole and toDouble() then handed back 0 for every ranged cost.
    static const QString pat = R"(\d+(?:\.\d+)?(?:[eE][-+]?\d+)?)";
    return pat;
}

const QRegularExpression &nodeRe()
{
    static const QRegularExpression re(R"(^(\s*)-> (.*)$)");
    return re;
}

const QRegularExpression &estRe()
{
    static const QRegularExpression re(
        R"(\s*\(cost=()" + numPat() + R"()(?:\.\.)" + numPat() + R"()?\s+rows=()" + numPat() +
        R"()\))"
    );
    return re;
}

const QRegularExpression &actualRe()
{
    static const QRegularExpression re(
        R"(\s*\(actual time=()" + numPat() + R"()\.\.()" + numPat() + R"()\s+rows=()" + numPat() +
        R"()\s+loops=()" + numPat() + R"()\))"
    );
    return re;
}

const QRegularExpression &neverRe()
{
    static const QRegularExpression re(R"(\s*\(never executed\))");
    return re;
}

PlanNode parseNode(const QString &rest)
{
    PlanNode n;
    QString name = rest;

    const auto est = estRe().match(name);
    if (est.hasMatch())
    {
        n.estCost = est.captured(1).toDouble();
        n.estRows = est.captured(2).toDouble();
        name.remove(estRe());
    }
    const auto act = actualRe().match(name);
    if (act.hasMatch())
    {
        n.actualFirstMs = act.captured(1).toDouble();
        n.actualLastMs = act.captured(2).toDouble();
        n.actualRows = act.captured(3).toDouble();
        n.loops = act.captured(4).toDouble();
        name.remove(actualRe());
    }
    if (neverRe().match(name).hasMatch())
    {
        n.neverExecuted = true;
        name.remove(neverRe());
    }
    n.name = name.trimmed();

    if (n.estRows && n.actualRows && *n.estRows > 0)
    {
        const double ratio =
            qMax(*n.actualRows / *n.estRows, *n.estRows / qMax(*n.actualRows, 1e-9));
        n.misestimate = ratio >= 100;
    }
    return n;
}

QString precision3(double v)
{
    return QString::number(v, 'g', 3);
}

} // namespace

QVector<PlanNode> parsePlanTree(const QString &text)
{
    QVector<PlanNode> roots;

    // (depth, path) stack; a new node attaches to the nearest shallower entry.
    // Paths index into roots so the vector can grow without dangling pointers.
    struct Frame
    {
        int depth;
        QVector<int> path;
    };

    QVector<Frame> stack;

    auto at = [&roots](const QVector<int> &path) -> PlanNode *
    {
        PlanNode *n = &roots[path.first()];
        for (int i = 1; i < path.size(); ++i)
        {
            n = &n->children[path.at(i)];
        }
        return n;
    };

    for (const QString &line : text.split('\n'))
    {
        const auto m = nodeRe().match(line);
        if (!m.hasMatch())
        {
            // Continuation text (not seen in practice; kept so nothing is lost).
            if (!stack.isEmpty() && !line.trimmed().isEmpty())
            {
                at(stack.last().path)->name += " " + line.trimmed();
            }
            continue;
        }
        const int depth = int(m.captured(1).size());
        PlanNode node = parseNode(m.captured(2));
        while (!stack.isEmpty() && stack.last().depth >= depth)
        {
            stack.removeLast();
        }
        QVector<int> path;
        if (stack.isEmpty())
        {
            roots.append(std::move(node));
            path = {int(roots.size()) - 1};
        }
        else
        {
            path = stack.last().path;
            PlanNode *parent = at(path);
            parent->children.append(std::move(node));
            path.append(int(parent->children.size()) - 1);
        }
        stack.append({depth, path});
    }
    return roots;
}

QString fmtNum(double n)
{
    if (!qIsFinite(n))
    {
        return QString::number(n);
    }
    if (n >= 1e9)
    {
        return precision3(n / 1e9) + "G";
    }
    if (n >= 1e6)
    {
        return precision3(n / 1e6) + "M";
    }
    if (n >= 1e3)
    {
        return precision3(n / 1e3) + "k";
    }
    if (qFuzzyCompare(n, qRound(n)))
    {
        return QString::number(qint64(n));
    }
    return precision3(n);
}

QString fmtMs(double ms)
{
    if (ms >= 1000)
    {
        return precision3(ms / 1000) + "s";
    }
    if (ms >= 1)
    {
        return precision3(ms) + "ms";
    }
    return precision3(ms * 1000) + "µs";
}

PlanView::PlanView(QWidget *parent) : QWidget(parent)
{
    const AppPalette &pal = theme::current();

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(12, 6, 12, 6);
    auto *title = new QLabel(tr("Execution Plan"));
    title->setProperty("muted", true);
    auto *copy = new QPushButton(tr("Copy Raw"));
    connect(
        copy, &QPushButton::clicked, this,
        [this, copy]()
        {
            QApplication::clipboard()->setText(m_text);
            copy->setText(tr("Copied"));
            QTimer::singleShot(1500, copy, [copy]() { copy->setText(tr("Copy Raw")); });
        }
    );
    bar->addWidget(title);
    // Stretch between them: the label reads on the left, the button sits at
    // the bar's right edge instead of glued to the text.
    bar->addStretch();
    bar->addWidget(copy);
    root->addLayout(bar);

    m_tree = new QTreeWidget;
    m_tree->setHeaderLabels({tr("Operation"), tr("Estimated"), tr("Actual")});
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(false);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

    m_raw = new QPlainTextEdit;
    m_raw->setReadOnly(true);
    m_raw->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_raw->setFrameShape(QFrame::NoFrame);

    m_stack = new QStackedWidget;
    m_stack->addWidget(m_tree);
    m_stack->addWidget(m_raw);
    root->addWidget(m_stack, 1);

    Q_UNUSED(pal);
}

// NOLINTNEXTLINE(misc-no-recursion) — a tree walk; depth is the plan tree's
void PlanView::addNode(const PlanNode &n, QTreeWidgetItem *parent)
{
    const AppPalette &pal = theme::current();
    auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
    item->setText(0, n.name);
    item->setToolTip(0, n.name);

    if (n.estRows)
    {
        QString s = "~" + fmtNum(*n.estRows) + " rows";
        if (n.estCost)
        {
            s += " · cost " + fmtNum(*n.estCost);
        }
        item->setText(1, s);
        item->setForeground(1, n.misestimate ? pal.warning : pal.mutedFg);
        item->setToolTip(
            1, tr("Optimizer estimate%1")
                   .arg(n.misestimate ? tr(" — off from actual by ≥100×") : QString())
        );
    }
    if (n.neverExecuted)
    {
        item->setText(2, tr("never executed"));
        item->setForeground(2, pal.mutedFg);
    }
    else if (n.actualLastMs)
    {
        QString s = fmtMs(*n.actualLastMs) + " · " + fmtNum(n.actualRows.value_or(0)) + " rows";
        const double loops = n.loops.value_or(1);
        if (loops > 1)
        {
            s += " ×" + fmtNum(loops);
        }
        item->setText(2, s);
        item->setForeground(2, n.misestimate ? pal.warning : pal.success);
        item->setToolTip(
            2, tr("Actual — first row %1, all rows %2, per loop")
                   .arg(fmtMs(n.actualFirstMs.value_or(0)), fmtMs(*n.actualLastMs))
        );
    }
    for (const PlanNode &c : n.children)
    {
        addNode(c, item);
    }
}

void PlanView::setPlan(const QString &text)
{
    m_text = text;
    m_tree->clear();
    const QVector<PlanNode> roots = parsePlanTree(text);
    if (roots.isEmpty())
    {
        // Not TREE-shaped (older server / MariaDB / FORMAT override).
        m_raw->setPlainText(text);
        m_stack->setCurrentWidget(m_raw);
        return;
    }
    for (const PlanNode &r : roots)
    {
        addNode(r, nullptr);
    }
    m_tree->expandAll();
    m_stack->setCurrentWidget(m_tree);
}
