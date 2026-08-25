#include "app/icons.h"

#include <QApplication>
#include <QFile>
#include <QHash>
#include <QPainter>
#include <QSvgRenderer>

namespace
{

// Keyed by name, colour, size and dpr — a repainted tree asks for the same
// handful of icons thousands of times.
QHash<QString, QPixmap> g_cache;

} // namespace

namespace icons
{

QPixmap pixmap(const QString &name, const QColor &colour, int px)
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    const QString key =
        name + '|' + colour.name() + '|' + QString::number(px) + '|' + QString::number(dpr, 'f', 2);
    const auto cached = g_cache.constFind(key);
    if (cached != g_cache.constEnd())
    {
        return *cached;
    }

    QFile f(":/icons/" + name + ".svg");
    if (!f.open(QIODevice::ReadOnly))
    {
        return {};
    }
    QByteArray svg = f.readAll();
    svg.replace("currentColor", colour.name().toUtf8());

    QSvgRenderer renderer(svg);
    QPixmap pm(QSize(px, px) * dpr);
    pm.fill(Qt::transparent);
    pm.setDevicePixelRatio(dpr);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    renderer.render(&p, QRectF(0, 0, px, px));
    p.end();

    g_cache.insert(key, pm);
    return pm;
}

QIcon icon(const QString &name, const QColor &colour, int px)
{
    return QIcon(pixmap(name, colour, px));
}

} // namespace icons
