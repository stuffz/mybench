#include "views/graphcanvas.h"

#include "app/theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace
{

// Same hash the web build uses, so a schema keeps its colour across both.
int schemaHue(const QString &s)
{
    int h = 0;
    for (const QChar &ch : s)
    {
        h = (h * 31 + ch.unicode()) % 360;
    }
    return h;
}

double clusterRad(int c)
{
    return 20.0 * std::sqrt(double(c)) + 30.0;
}

constexpr double GoldenAngle = 2.399963;

} // namespace

GraphCanvas::GraphCanvas(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(true);

    // Idle-CPU rule: the timer only runs while the simulation is hot; once it
    // settles we repaint on input alone.
    m_timer = new QTimer(this);
    m_timer->setInterval(16);
    connect(
        m_timer, &QTimer::timeout, this,
        [this]()
        {
            if (m_alpha <= 0.005)
            {
                m_timer->stop();
                return;
            }
            step();
            update();
        }
    );
}

void GraphCanvas::setGraph(
    const QVector<QVector<QString>> &nodes, const QVector<QPair<QString, QString>> &edges
)
{
    m_raw.clear();
    for (const QVector<QString> &n : nodes)
    {
        RawNode r;
        r.schema = n.value(0);
        r.table = n.value(1);
        r.rows = n.value(2).toLongLong();
        m_raw.append(r);
    }
    m_rawEdges = edges;
    rebuild();
}

void GraphCanvas::setMaxNodeSize(int px)
{
    m_maxNode = px;
    // Only the radii change: a full rebuild would re-spiral every node and
    // discard the user's pan, zoom, and hand-dragged positions mid-drag.
    double maxLog = 1;
    for (const Node &n : m_nodes)
    {
        maxLog = qMax(maxLog, std::log10(double(n.rows) + 1));
    }
    for (Node &n : m_nodes)
    {
        const double size =
            4 + (m_maxNode - 4) * (std::log10(double(n.rows) + 1) / maxLog) + qMin(n.deg, 8) * 0.5;
        n.r = qBound(3.5, size, double(m_maxNode));
    }
    m_alpha = qMax(m_alpha, 0.2); // gentle reflow: springs rest on the radii
    if (!m_nodes.isEmpty())
    {
        m_timer->start();
    }
    update();
}

void GraphCanvas::setHideIsolated(bool on)
{
    m_hideIsolated = on;
    rebuild();
}

void GraphCanvas::setFocusTable(const QString &id, int hops)
{
    m_focus = id;
    m_hops = hops;
    rebuild();
}

void GraphCanvas::fit()
{
    m_userMoved = false;
    update();
}

QStringList GraphCanvas::nodeIds() const
{
    QStringList out;
    out.reserve(m_raw.size());
    for (const RawNode &n : m_raw)
    {
        out << n.schema + "." + n.table;
    }
    return out;
}

void GraphCanvas::rebuild()
{
    m_nodes.clear();
    m_links.clear();
    m_neighbors.clear();
    m_schemaList.clear();
    m_schemaCounts.clear();

    // Only edges that will actually be drawn count towards a table being
    // connected: a self reference or an endpoint outside the fetched set
    // leaves a node as alone on screen as one with no edge at all.
    QSet<QString> present;
    present.reserve(int(m_raw.size()));
    for (const RawNode &n : m_raw)
    {
        present.insert(n.schema + "." + n.table);
    }
    QHash<QString, int> degree;
    for (const auto &e : m_rawEdges)
    {
        if (e.first == e.second || !present.contains(e.first) || !present.contains(e.second))
        {
            continue;
        }
        degree[e.first]++;
        degree[e.second]++;
    }

    // Focus mode: BFS over FK edges (undirected) up to `hops` jumps. While
    // focused, "hide isolated" is moot — the reach set is the filter.
    QVector<RawNode> visible;
    const QStringList ids = nodeIds();
    const bool focusExists = !m_focus.isEmpty() && ids.contains(m_focus);
    if (focusExists)
    {
        QHash<QString, QSet<QString>> adj;
        for (const auto &e : m_rawEdges)
        {
            adj[e.first].insert(e.second);
            adj[e.second].insert(e.first);
        }
        QSet<QString> reach{m_focus};
        QStringList frontier{m_focus};
        for (int d = 0; d < m_hops && !frontier.isEmpty(); ++d)
        {
            QStringList next;
            for (const QString &id : frontier)
            {
                for (const QString &nb : adj.value(id))
                {
                    if (!reach.contains(nb))
                    {
                        reach.insert(nb);
                        next << nb;
                    }
                }
            }
            frontier = next;
        }
        for (const RawNode &n : m_raw)
        {
            if (reach.contains(n.schema + "." + n.table))
            {
                visible.append(n);
            }
        }
    }
    else if (m_hideIsolated)
    {
        for (const RawNode &n : m_raw)
        {
            if (degree.value(n.schema + "." + n.table) > 0)
            {
                visible.append(n);
            }
        }
    }
    else
    {
        visible = m_raw;
    }

    // Radius scales relative to the largest visible table, so small and huge
    // databases both spread across the whole size range.
    double maxLog = 1;
    for (const RawNode &n : visible)
    {
        maxLog = qMax(maxLog, std::log10(double(n.rows) + 1));
    }

    for (const RawNode &n : visible)
    {
        m_schemaCounts[n.schema]++;
    }
    m_schemaList = m_schemaCounts.keys();
    std::sort(m_schemaList.begin(), m_schemaList.end());

    // Schemas sit on a ring sized to their table counts, each with its own
    // spiral, and gravity pulls nodes to their schema anchor — grouped
    // clusters instead of one interleaved hairball.
    double circum = 0;
    for (const QString &s : m_schemaList)
    {
        circum += 2 * clusterRad(m_schemaCounts.value(s)) + 60;
    }
    const double ringR = m_schemaList.size() > 1 ? qMax(220.0, circum / (2 * std::numbers::pi)) : 0;
    QHash<QString, QPointF> anchors;
    double acc = 0;
    for (const QString &s : m_schemaList)
    {
        const double span = (2 * clusterRad(m_schemaCounts.value(s)) + 60) / qMax(circum, 1.0);
        const double a = (acc + span / 2) * 2 * std::numbers::pi;
        anchors.insert(s, QPointF(std::cos(a) * ringR, std::sin(a) * ringR));
        acc += span;
    }

    QHash<QString, int> withinIdx;
    QHash<QString, int> indexOfId;
    for (const RawNode &rn : visible)
    {
        const QString id = rn.schema + "." + rn.table;
        const QPointF anchor = anchors.value(rn.schema);
        const int deg = degree.value(id);
        // Deterministic spiral within the cluster — stable layout run-to-run.
        const int j = withinIdx.value(rn.schema, 0);
        withinIdx.insert(rn.schema, j + 1);
        const double angle = j * GoldenAngle;
        const double rad = 14 * std::sqrt(double(j) + 1);
        const double size =
            4 + (m_maxNode - 4) * (std::log10(double(rn.rows) + 1) / maxLog) + qMin(deg, 8) * 0.5;

        Node n;
        n.id = id;
        n.schema = rn.schema;
        n.table = rn.table;
        n.x = anchor.x() + std::cos(angle) * rad;
        n.y = anchor.y() + std::sin(angle) * rad;
        n.ax = anchor.x();
        n.ay = anchor.y();
        n.r = qBound(3.5, size, double(m_maxNode));
        n.rows = rn.rows;
        n.hue = schemaHue(rn.schema);
        n.deg = deg;
        indexOfId.insert(id, int(m_nodes.size()));
        m_nodes.append(n);
    }

    for (const auto &e : m_rawEdges)
    {
        const int a = indexOfId.value(e.first, -1);
        const int b = indexOfId.value(e.second, -1);
        if (a < 0 || b < 0 || a == b)
        {
            continue;
        }
        m_links.append({a, b});
        m_neighbors[a].insert(b);
        m_neighbors[b].insert(a);
    }

    m_alpha = 1;
    m_userMoved = false;
    m_hovered = -1;
    emit countsChanged(int(m_nodes.size()), int(m_links.size()));
    if (!m_nodes.isEmpty())
    {
        m_timer->start();
    }
    update();
}

void GraphCanvas::step()
{
    const int n = int(m_nodes.size());
    // Repulsion (capped O(n²) — fine at this scale).
    for (int i = 0; i < n; ++i)
    {
        Node &a = m_nodes[i];
        for (int j = i + 1; j < n; ++j)
        {
            Node &b = m_nodes[j];
            double dx = a.x - b.x;
            double dy = a.y - b.y;
            double d2 = dx * dx + dy * dy;
            if (d2 < 1)
            {
                dx = double((i * 7919) % 13) - 6;
                dy = double((j * 104729) % 13) - 6;
                d2 = dx * dx + dy * dy;
                if (d2 == 0)
                {
                    d2 = 1;
                }
            }
            if (d2 > 250000)
            {
                continue;
            }
            const double f = (2200.0 / d2) * m_alpha;
            const double d = std::sqrt(d2);
            const double fx = dx / d * f;
            const double fy = dy / d * f;
            a.vx += fx;
            a.vy += fy;
            b.vx -= fx;
            b.vy -= fy;
        }
    }
    // FK springs — rest length scales with the node sizes.
    for (const Link &l : m_links)
    {
        Node &a = m_nodes[l.a];
        Node &b = m_nodes[l.b];
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        double d = std::sqrt(dx * dx + dy * dy);
        if (d == 0)
        {
            d = 1;
        }
        const double f = (d - (60 + a.r + b.r)) * 0.02 * m_alpha;
        const double fx = dx / d * f;
        const double fy = dy / d * f;
        a.vx += fx;
        a.vy += fy;
        b.vx -= fx;
        b.vy -= fy;
    }
    // Cluster gravity + integrate.
    for (int i = 0; i < n; ++i)
    {
        Node &nd = m_nodes[i];
        nd.vx += (nd.ax - nd.x) * 0.0045 * m_alpha;
        nd.vy += (nd.ay - nd.y) * 0.0045 * m_alpha;
        if (i == m_dragNode)
        {
            nd.vx = 0;
            nd.vy = 0;
            continue;
        }
        nd.vx *= 0.85;
        nd.vy *= 0.85;
        nd.x += nd.vx;
        nd.y += nd.vy;
    }
    // Large graphs decay faster: the repulsion pass is O(n²) per frame, so
    // capping the number of hot frames bounds the cost on huge schemas.
    m_alpha *= n > 800 ? 0.99 : 0.995;
}

QPointF GraphCanvas::toWorld(const QPointF &s) const
{
    return QPointF((s.x() - m_tx) / m_k, (s.y() - m_ty) / m_k);
}

int GraphCanvas::hitTest(const QPointF &screen) const
{
    const QPointF w = toWorld(screen);
    for (int i = int(m_nodes.size()) - 1; i >= 0; --i)
    {
        const Node &n = m_nodes.at(i);
        const double dx = n.x - w.x();
        const double dy = n.y - w.y();
        const double rr = n.r + 3 / m_k;
        if (dx * dx + dy * dy <= rr * rr)
        {
            return i;
        }
    }
    return -1;
}

void GraphCanvas::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    if (m_tx == 0 && m_ty == 0)
    {
        m_tx = width() / 2.0;
        m_ty = height() / 2.0;
    }
}

void GraphCanvas::paintEvent(QPaintEvent *)
{
    const AppPalette &pal = theme::current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), pal.background);
    if (m_nodes.isEmpty())
    {
        return;
    }

    // Auto fit-to-view until the user pans, zooms or drags; Fit re-enables it.
    if (!m_userMoved)
    {
        double minX = std::numeric_limits<double>::max(), maxX = -minX;
        double minY = minX, maxY = -minX;
        for (const Node &n : m_nodes)
        {
            minX = qMin(minX, n.x - n.r);
            maxX = qMax(maxX, n.x + n.r);
            minY = qMin(minY, n.y - n.r);
            maxY = qMax(maxY, n.y + n.r);
        }
        const double bw = qMax(1.0, maxX - minX);
        const double bh = qMax(1.0, maxY - minY);
        m_k = qMin(1.4, qMin(width() / bw, height() / bh) * 0.9);
        m_tx = width() / 2.0 - (minX + maxX) / 2.0 * m_k;
        m_ty = height() / 2.0 - (minY + maxY) / 2.0 * m_k;
    }

    p.translate(m_tx, m_ty);
    p.scale(m_k, m_k);

    const QSet<int> hoverSet = m_hovered >= 0 ? m_neighbors.value(m_hovered) : QSet<int>();
    const bool dimmed = m_hovered >= 0;

    // Edges
    for (const Link &l : m_links)
    {
        const bool lit = dimmed && (l.a == m_hovered || l.b == m_hovered);
        QColor c = lit ? pal.info : pal.mutedFg;
        c.setAlphaF(lit ? 0.9f : (dimmed ? 0.08f : 0.28f));
        p.setPen(QPen(c, 1.0 / m_k));
        p.drawLine(
            QPointF(m_nodes.at(l.a).x, m_nodes.at(l.a).y),
            QPointF(m_nodes.at(l.b).x, m_nodes.at(l.b).y)
        );
    }

    // Nodes — fills keep their data-derived hue so schemas stay separable.
    p.setPen(Qt::NoPen);
    for (int i = 0; i < m_nodes.size(); ++i)
    {
        const Node &n = m_nodes.at(i);
        const bool hoverFocus = i == m_hovered || hoverSet.contains(i);
        QColor c = QColor::fromHslF(float(n.hue) / 360.0f, 0.55f, i == m_hovered ? 0.68f : 0.56f);
        c.setAlphaF(dimmed && !hoverFocus ? 0.15 : 0.92);
        p.setBrush(c);
        p.drawEllipse(QPointF(n.x, n.y), n.r, n.r);
        if (n.id == m_focus)
        {
            // Ring marks the focused table.
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(pal.foreground, 2.0 / m_k));
            p.drawEllipse(QPointF(n.x, n.y), n.r, n.r);
            p.setPen(Qt::NoPen);
        }
    }

    // Labels
    const bool labelAll = m_k > 1.4 || m_nodes.size() <= 60;
    // px, scaled by the zoom: point sizes here drifted with the screen DPI and
    // ignored the prefs slider.
    QFont f(theme::monoFamily());
    f.setPixelSize(qMax(1, int(std::lround(theme::scaledPx(0.85) / m_k))));
    p.setFont(f);
    for (int i = 0; i < m_nodes.size(); ++i)
    {
        const Node &n = m_nodes.at(i);
        const bool focused = i == m_hovered || hoverSet.contains(i);
        if ((!labelAll && !focused) || (dimmed && !focused))
        {
            continue;
        }
        QColor c = focused ? pal.foreground : pal.mutedFg;
        c.setAlphaF(focused ? 0.95 : 0.7);
        p.setPen(c);
        const QString text = i == m_hovered ? n.id : n.table;
        const QFontMetricsF fm(f);
        p.drawText(QPointF(n.x - fm.horizontalAdvance(text) / 2, n.y - n.r - 4 / m_k), text);
    }

    // Zoomed out, name the clusters instead of the unreadable tables.
    if (m_k < 1.1 && m_schemaList.size() > 1 && !dimmed)
    {
        QHash<QString, QPointF> sums;
        QHash<QString, int> counts;
        for (const Node &n : m_nodes)
        {
            sums[n.schema] += QPointF(n.x, n.y);
            counts[n.schema]++;
        }
        QFont sf(theme::monoFamily());
        sf.setPixelSize(qBound(1, int(std::lround(theme::uiFontSize() / m_k)), 44));
        p.setFont(sf);
        const QFontMetricsF fm(sf);
        for (auto it = sums.constBegin(); it != sums.constEnd(); ++it)
        {
            const int c = counts.value(it.key());
            QColor col = QColor::fromHslF(float(schemaHue(it.key())) / 360.0f, 0.45f, 0.72f);
            col.setAlphaF(0.85);
            p.setPen(col);
            const QPointF centre = it.value() / c;
            p.drawText(
                QPointF(
                    centre.x() - fm.horizontalAdvance(it.key()) / 2,
                    centre.y() - clusterRad(c) - 8 / m_k
                ),
                it.key()
            );
        }
    }
}

void GraphCanvas::wheelEvent(QWheelEvent *e)
{
    m_userMoved = true;
    const QPointF s = e->position();
    const QPointF w = toWorld(s);
    m_k = qBound(0.08, m_k * std::exp(-e->angleDelta().y() * -0.0012), 6.0);
    m_tx = s.x() - w.x() * m_k;
    m_ty = s.y() - w.y() * m_k;
    update();
}

void GraphCanvas::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
    {
        return;
    }
    m_last = e->position();
    const int hit = hitTest(e->position());
    if (hit >= 0)
    {
        m_dragNode = hit;
        m_alpha = qMax(m_alpha, 0.35); // nudge the sim so the graph reflows
        m_timer->start();
    }
    else
    {
        m_panning = true;
        setCursor(Qt::ClosedHandCursor);
    }
    m_userMoved = true;
}

void GraphCanvas::mouseMoveEvent(QMouseEvent *e)
{
    const QPointF pos = e->position();
    if (m_dragNode >= 0)
    {
        const QPointF w = toWorld(pos);
        m_nodes[m_dragNode].x = w.x();
        m_nodes[m_dragNode].y = w.y();
        update();
        return;
    }
    if (m_panning)
    {
        m_tx += pos.x() - m_last.x();
        m_ty += pos.y() - m_last.y();
        m_last = pos;
        update();
        return;
    }
    const int hit = hitTest(pos);
    if (hit != m_hovered)
    {
        m_hovered = hit;
        setToolTip(
            hit >= 0 ? m_nodes.at(hit).id + tr("\n%1 foreign keys").arg(m_nodes.at(hit).deg)
                     : QString()
        );
        update();
    }
}

void GraphCanvas::mouseReleaseEvent(QMouseEvent *)
{
    m_dragNode = -1;
    m_panning = false;
    unsetCursor();
}

void GraphCanvas::mouseDoubleClickEvent(QMouseEvent *e)
{
    const int hit = hitTest(e->position());
    if (hit >= 0)
    {
        emit tableActivated(m_nodes.at(hit).schema, m_nodes.at(hit).table);
    }
}
