#include "shell/statusstrip.h"

#include "app/api.h"
#include "app/icons.h"
#include "app/theme.h"
#include "ui/fmt.h"
#include "ui/widgets.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QTimer>

namespace
{

constexpr int StatusPollMs = 5000;

// One status-strip item: lucide glyph then value, sharing a tooltip.
QWidget *metricItem(const QString &iconName, QLabel *value, const QString &tip)
{
    auto *row = new QWidget;
    auto *l = new QHBoxLayout(row);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(5);
    auto *glyph = new QLabel;
    glyph->setObjectName("glyph_" + iconName);
    glyph->setPixmap(icons::pixmap(iconName, theme::current().mutedFg, 12));
    l->addWidget(glyph);
    l->addWidget(value);
    for (QWidget *w : {row, static_cast<QWidget *>(glyph), static_cast<QWidget *>(value)})
    {
        w->setToolTip(tip);
    }
    return row;
}

// The dot, the latency text and their container all carry the tip; hovering
// any part of the item should show it.
void setGroupToolTip(QWidget *group, const QString &tip)
{
    group->setToolTip(tip);
    for (QObject *child : group->children())
    {
        if (auto *w = qobject_cast<QWidget *>(child))
        {
            w->setToolTip(tip);
        }
    }
}

} // namespace

StatusStrip::StatusStrip(QWidget *parent) : QWidget(parent)
{
    const AppPalette &pal = theme::current();
    setAttribute(Qt::WA_StyledBackground, true); // else the border-top never paints
    setStyleSheet(QString("StatusStrip { border-top: 1px solid %1; }").arg(pal.border.name()));
    setFixedHeight(28);

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(12, 0, 12, 0);
    row->setSpacing(16);

    m_dot = new QLabel;
    m_dot->setFixedSize(8, 8);
    m_latency = smallLabel();
    m_version = smallLabel();
    m_uptime = smallLabel();
    m_threads = smallLabel();
    m_qps = smallLabel();
    m_error = smallLabel();
    m_error->setProperty("tone", "destructive");
    m_error->setStyleSheet(QString("QLabel { color: %1; }").arg(pal.destructive.name()));

    m_health = new QWidget;
    auto *hl = new QHBoxLayout(m_health);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(6);
    hl->addWidget(m_dot);
    hl->addWidget(m_latency);
    row->addWidget(m_health);
    row->addWidget(metricItem("database", m_version, tr("MySQL Server Version")));
    row->addWidget(metricItem("clock", m_uptime, tr("Uptime")));
    row->addWidget(metricItem("cpu", m_threads, tr("Threads Running / Connected")));
    row->addWidget(metricItem("activity", m_qps, tr("Queries per Second")));
    row->addWidget(m_error, 1);
    row->addStretch();

    m_timer = new QTimer(this);
    m_timer->setInterval(StatusPollMs);
    connect(m_timer, &QTimer::timeout, this, &StatusStrip::tick);
}

void StatusStrip::applyTheme()
{
    const AppPalette &pal = theme::current();
    setStyleSheet(QString("StatusStrip { border-top: 1px solid %1; }").arg(pal.border.name()));
    m_error->setStyleSheet(QString("QLabel { color: %1; }").arg(pal.destructive.name()));
    // The dot is repainted by the next poll; the glyphs are not, so redo them.
    for (QLabel *glyph : findChildren<QLabel *>())
    {
        const QString name = glyph->objectName();
        if (name.startsWith("glyph_"))
        {
            glyph->setPixmap(icons::pixmap(name.mid(6), pal.mutedFg, 12));
        }
    }
}

void StatusStrip::watch(const QString &connID, const QString &label)
{
    m_connID = connID;
    m_label = label;
    m_prevQuestions = -1;
    m_prevAt = 0;
    for (QLabel *l : {m_version, m_uptime, m_threads, m_qps, m_error, m_latency})
    {
        l->clear();
    }
    setGroupToolTip(m_health, label);
    if (connID.isEmpty())
    {
        m_timer->stop();
        setVisible(false);
        return;
    }
    setVisible(true);
    tick();
    m_timer->start();
}

void StatusStrip::tick()
{
    const QString conn = m_connID;
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    api()->call(
        "admin", "GlobalStatus", {conn}, this,
        [this, conn, started](const QJsonValue &res, const QString &err)
        {
            if (conn != m_connID)
            {
                return;
            }
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const AppPalette &p = theme::current();
            const QString checked =
                tr(" · last check %1").arg(QDateTime::currentDateTime().toString("HH:mm:ss"));
            if (!err.isEmpty())
            {
                // Keep the last snapshot visible; the dot carries the bad news.
                m_dot->setStyleSheet(QString("QLabel { background: %1; border-radius: 4px; }")
                                         .arg(p.destructive.name()));
                m_latency->setText(tr("offline"));
                m_error->setText(err);
                m_error->setToolTip(err);
                setGroupToolTip(m_health, m_label + tr(" · unreachable: %1").arg(err) + checked);
                m_prevQuestions = -1;
                m_qps->clear();
                return;
            }
            const QJsonObject o = res.toObject();
            m_dot->setStyleSheet(
                QString("QLabel { background: %1; border-radius: 4px; }").arg(p.success.name())
            );
            m_error->clear();
            m_latency->setText(QString("%1ms").arg(now - started));
            m_version->setText(o.value("version").toString());
            m_uptime->setText(fmtUptime(qint64(o.value("uptimeSeconds").toDouble())));
            m_threads->setText(QString("%1/%2")
                                   .arg(o.value("threadsRunning").toInt())
                                   .arg(o.value("threadsConnected").toInt()));
            const double q = o.value("questions").toDouble();
            if (m_prevQuestions >= 0 && now > m_prevAt)
            {
                const double dq = q - m_prevQuestions;
                const double dt = double(now - m_prevAt) / 1000.0;
                if (dt > 0 && dq >= 0)
                {
                    m_qps->setText(QString("%1 qps").arg(qRound(dq / dt)));
                }
            }
            m_prevQuestions = q;
            m_prevAt = now;
            setGroupToolTip(
                m_health, m_label + tr(" · healthy · %1ms roundtrip").arg(now - started) + checked
            );
        }
    );
}
