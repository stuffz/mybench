#include "views/graphview.h"

#include "app/api.h"
#include "app/theme.h"
#include "editor/sqlscan.h" // fuzzyScore
#include "ui/switchbox.h"
#include "ui/widgets.h"
#include "views/graphcanvas.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QShortcut>
#include <QSlider>
#include <QVBoxLayout>
#include <algorithm>

GraphView::GraphView(const QString &connID, QWidget *parent) : QWidget(parent), m_connID(connID)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // --- toolbar: actions + find on the first row, view knobs + status on
    // the second — one row held too many controls to scan.
    auto *barWrap = new QWidget;
    auto *bars = new QVBoxLayout(barWrap);
    bars->setContentsMargins(12, 8, 12, 8);
    bars->setSpacing(6);
    auto *bar = new QHBoxLayout;
    bar->setSpacing(8);
    auto *bar2 = new QHBoxLayout;
    bar2->setSpacing(8);
    bars->addLayout(bar);
    bars->addLayout(bar2);

    // Reload the graph, reframe it, drop the tables nothing points at.
    auto *refreshBtn = new QPushButton(tr("Refresh"));
    connect(refreshBtn, &QPushButton::clicked, this, &GraphView::refresh);
    auto *fitBtn = new QPushButton(tr("Fit"));
    m_hideIsolated = new SwitchBox(tr("Hide Isolated"));

    // Find and focus.
    m_find = new QLineEdit;
    m_find->setPlaceholderText(tr("Find table…"));
    m_find->setMaximumWidth(220);
    m_clearFocus = new QPushButton(tr("Clear Focus"));
    m_clearFocus->setVisible(false);

    bar->addWidget(refreshBtn);
    bar->addWidget(fitBtn);
    bar->addWidget(vhairline());
    bar->addWidget(m_hideIsolated);
    bar->addStretch();
    bar->addWidget(m_find);
    bar->addWidget(m_clearFocus);

    // How much is drawn: the node scale, and how far a focus reaches. Hops
    // is a stepper, not a spinbox — six discrete values, one tap per step.
    m_size = new QSlider(Qt::Horizontal);
    m_size->setRange(6, 40);
    m_size->setValue(16);
    m_size->setMaximumWidth(110);

    auto *hopsMinus = new QPushButton(QStringLiteral("−"));
    auto *hopsPlus = new QPushButton(QStringLiteral("+"));
    m_hopsValue = new QLabel(QString::number(m_hopsN));
    m_hopsValue->setAlignment(Qt::AlignCenter);
    m_hopsValue->setMinimumWidth(theme::scaledPx(1.2));
    const QString hopsTip = tr("How far a focus reaches, in foreign-key hops (1–6)");
    hopsMinus->setToolTip(hopsTip);
    hopsPlus->setToolTip(hopsTip);
    m_hopsValue->setToolTip(hopsTip);
    for (QPushButton *b : {hopsMinus, hopsPlus})
    {
        b->setProperty("variant", "ghost");
        b->setObjectName("Stepper"); // zero padding, or the glyph clips
        b->setFixedWidth(theme::scaledPx(1.9));
    }
    connect(hopsMinus, &QPushButton::clicked, this, [this]() { setHops(m_hopsN - 1); });
    connect(hopsPlus, &QPushButton::clicked, this, [this]() { setHops(m_hopsN + 1); });

    m_counts = mutedLabel();
    m_error = new QLabel;

    bar2->addWidget(new QLabel(tr("Node Size")));
    bar2->addWidget(m_size);
    bar2->addWidget(vhairline());
    bar2->addWidget(new QLabel(tr("Hops")));
    bar2->addWidget(hopsMinus);
    bar2->addWidget(m_hopsValue);
    bar2->addWidget(hopsPlus);
    bar2->addStretch();
    bar2->addWidget(m_counts);
    bar2->addWidget(m_error);
    // Re-styled on theme change; baked-at-construction colour would go stale.
    const auto restyleError = [this]()
    {
        m_error->setStyleSheet(
            QString("QLabel { color: %1; }").arg(theme::current().destructive.name())
        );
    };
    restyleError();
    connect(theme::notifier(), &Notifier::changed, this, restyleError);

    root->addWidget(barWrap);

    m_canvas = new GraphCanvas;
    root->addWidget(m_canvas, 1);

    // The find list floats as a small popup list under the box.
    m_findList = new QListWidget(this);
    m_findList->setVisible(false);
    m_findList->setMaximumHeight(180);

    connect(fitBtn, &QPushButton::clicked, m_canvas, &GraphCanvas::fit);
    connect(m_hideIsolated, &QCheckBox::toggled, m_canvas, &GraphCanvas::setHideIsolated);
    connect(m_size, &QSlider::valueChanged, m_canvas, &GraphCanvas::setMaxNodeSize);
    connect(m_find, &QLineEdit::textChanged, this, &GraphView::runFind);
    connect(
        m_findList, &QListWidget::itemActivated, this,
        [this](QListWidgetItem *it)
        {
            const QString id = it->text();
            m_canvas->setFocusTable(id, m_hopsN);
            m_clearFocus->setVisible(true);
            m_findList->setVisible(false);
            m_find->clear();
        }
    );
    connect(m_findList, &QListWidget::itemClicked, m_findList, &QListWidget::itemActivated);
    // Enter in the box takes the highlighted hit, and Down moves into the list,
    // so the find flow works without reaching for the mouse.
    connect(
        m_find, &QLineEdit::returnPressed, this,
        [this]()
        {
            if (m_findList->isVisible() && m_findList->currentItem())
            {
                emit m_findList->itemActivated(m_findList->currentItem());
            }
        }
    );
    auto *down = new QShortcut(Qt::Key_Down, m_find);
    down->setContext(Qt::WidgetShortcut);
    connect(
        down, &QShortcut::activated, this,
        [this]()
        {
            if (!m_findList->isVisible())
            {
                return;
            }
            m_findList->setFocus();
            if (m_findList->currentRow() + 1 < m_findList->count())
            {
                m_findList->setCurrentRow(m_findList->currentRow() + 1);
            }
        }
    );
    connect(
        m_clearFocus, &QPushButton::clicked, this,
        [this]()
        {
            m_canvas->setFocusTable(QString(), m_hopsN);
            m_clearFocus->setVisible(false);
        }
    );
    // Double-clicking a node focuses it — the same reach-set filter the find
    // list and "Show in Graph" apply.
    connect(
        m_canvas, &GraphCanvas::tableActivated, this,
        [this](const QString &schema, const QString &table) { focusOn(schema, table); }
    );
    connect(
        m_canvas, &GraphCanvas::countsChanged, this,
        [this](int n, int e) { m_counts->setText(tr("%1 tables · %2 foreign keys").arg(n).arg(e)); }
    );

    refresh();
}

void GraphView::focusOn(const QString &schema, const QString &table)
{
    const QString id = schema + "." + table;
    m_canvas->setFocusTable(id, m_hopsN);
    m_clearFocus->setVisible(true);
}

// setHops clamps to the stepper's 1–6 and re-applies the current focus so the
// reach-set follows the new radius immediately.
void GraphView::setHops(int h)
{
    h = qBound(1, h, 6);
    if (h == m_hopsN)
    {
        return;
    }
    m_hopsN = h;
    m_hopsValue->setText(QString::number(h));
    m_canvas->setFocusTable(m_canvas->focusTable(), h);
}

void GraphView::runFind()
{
    const QString q = m_find->text().trimmed();
    if (q.isEmpty())
    {
        m_findList->setVisible(false);
        return;
    }

    struct Hit
    {
        int score;
        QString id;
    };

    QVector<Hit> hits;
    for (const QString &id : m_canvas->nodeIds())
    {
        int s = 0;
        if (fuzzyScore(q, id, &s))
        {
            hits.append({s, id});
        }
    }
    std::sort(
        hits.begin(), hits.end(), [](const Hit &a, const Hit &b) { return a.score > b.score; }
    );
    m_findList->clear();
    for (int i = 0; i < qMin<qsizetype>(hits.size(), 12); ++i)
    {
        m_findList->addItem(hits.at(i).id);
    }
    if (m_findList->count() == 0)
    {
        m_findList->setVisible(false);
        return;
    }
    m_findList->setCurrentRow(0);
    const QPoint at = m_find->mapTo(this, m_find->rect().bottomLeft());
    m_findList->setGeometry(at.x(), at.y() + 2, qMax(260, m_find->width()), 180);
    m_findList->raise();
    m_findList->setVisible(true);
}

void GraphView::refresh()
{
    api()->call(
        "admin", "FKGraph", {m_connID}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                m_error->setText(err);
                return;
            }
            m_error->clear();
            const QJsonObject o = res.toObject();
            QVector<QVector<QString>> nodes;
            for (const auto &v : o.value("nodes").toArray())
            {
                const QJsonObject n = v.toObject();
                nodes.append(
                    {n.value("schema").toString(), n.value("table").toString(),
                     QString::number(qint64(n.value("rows").toDouble()))}
                );
            }
            QVector<QPair<QString, QString>> edges;
            for (const auto &v : o.value("edges").toArray())
            {
                const QJsonObject e = v.toObject();
                edges.append(
                    {e.value("fromSchema").toString() + "." + e.value("fromTable").toString(),
                     e.value("toSchema").toString() + "." + e.value("toTable").toString()}
                );
            }
            m_canvas->setGraph(nodes, edges);
        }
    );
}
