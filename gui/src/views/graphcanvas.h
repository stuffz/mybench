#pragma once
// The painted half of the whole-server FK graph, Obsidian-style: every user
// table a node, every foreign key an edge. Hand-rolled force layout, painted
// with QPainter — the same simulation the web canvas ran, with the same
// constants, so the layout is recognisably the same graph. The chrome around
// it lives in GraphView (graphview.h).
#include <QHash>
#include <QVector>
#include <QWidget>

class QTimer;

class GraphCanvas : public QWidget
{
    Q_OBJECT
public:
    explicit GraphCanvas(QWidget *parent = nullptr);

    struct Node
    {
        QString id, schema, table;
        double x = 0, y = 0, vx = 0, vy = 0, r = 6;
        double ax = 0, ay = 0; // schema-cluster anchor
        qint64 rows = 0;       // kept so a size-slider change can resize in place
        int hue = 0;
        int deg = 0;
    };

    struct Link
    {
        int a = 0, b = 0;
    };

    void
    setGraph(const QVector<QVector<QString>> &nodes, const QVector<QPair<QString, QString>> &edges);
    void setMaxNodeSize(int px);
    void setHideIsolated(bool on);
    // Not setFocus(): that name belongs to QWidget.
    void setFocusTable(const QString &id, int hops);

    QString focusTable() const { return m_focus; }

    void fit();
    QStringList nodeIds() const;

    QPair<int, int> counts() const { return {int(m_nodes.size()), int(m_links.size())}; }

signals:
    void countsChanged(int nodes, int edges);
    void tableActivated(const QString &schema, const QString &table);

protected:
    void paintEvent(QPaintEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    void rebuild();
    void step();
    int hitTest(const QPointF &screen) const;
    QPointF toWorld(const QPointF &screen) const;

    // Raw graph, kept so filters can rebuild without refetching.
    struct RawNode
    {
        QString schema, table;
        qint64 rows = 0;
    };

    QVector<RawNode> m_raw;
    QVector<QPair<QString, QString>> m_rawEdges;

    QVector<Node> m_nodes;
    QVector<Link> m_links;
    QHash<int, QSet<int>> m_neighbors;
    QStringList m_schemaList;
    QHash<QString, int> m_schemaCounts;

    double m_k = 1, m_tx = 0, m_ty = 0, m_alpha = 1;
    int m_hovered = -1, m_dragNode = -1;
    bool m_panning = false, m_userMoved = false;
    QPointF m_last;
    int m_maxNode = 16;
    bool m_hideIsolated = false;
    QString m_focus;
    int m_hops = 1;
    QTimer *m_timer;
};
