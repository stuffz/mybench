#include "views/dashboardview.h"

#include "app/api.h"
#include "app/theme.h"
#include "ui/fmt.h"
#include "ui/tableutil.h"
#include "ui/widgets.h"
#include "views/chart.h"
#include "views/panelbase.h"
#include "views/statcard.h"

#include "ui/switchbox.h"
#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace
{

// The counters move fast enough that a slower poll draws a lie; two seconds is
// what the processlist panel already uses.
constexpr int PollMs = 2000;
// SHOW ENGINE INNODB STATUS is only read for the sections with no structured
// equivalent (deadlock, FK error, semaphores). Those change rarely, and the
// statement resets the monitor's own averaging window, so it is polled far
// more slowly than the counters.
constexpr int SectionPollMs = 15000;

constexpr int CardColumns = 4;
constexpr int ChartColumns = 2;
constexpr int TableHeight = 340;
constexpr int ListHeight = 176;

const char *const kDeadlockSection = "LATEST DETECTED DEADLOCK";
const char *const kForeignKeySection = "LATEST FOREIGN KEY ERROR";
const char *const kSemaphoreSection = "SEMAPHORES";

// Section headings inside the scrolling page.
QLabel *sectionLabel(const QString &text)
{
    QLabel *l = weightedLabel(text, QFont::DemiBold);
    l->setProperty("muted", true);
    return l;
}

QPlainTextEdit *textBox(int height)
{
    auto *t = new QPlainTextEdit;
    t->setObjectName("Plain");
    t->setReadOnly(true);
    t->setLineWrapMode(QPlainTextEdit::NoWrap);
    t->setFixedHeight(height);
    // Kept so a section that turns out to be empty can collapse to one line
    // and expand again when the server does report it.
    t->setProperty("fullHeight", height);
    return t;
}

} // namespace

DashboardView::DashboardView(const QString &connID, QWidget *parent)
    : PanelBase(tr("Server Dashboard"), connID, parent)
{
    // --- header: the sampling switch and the server line ------------------
    m_live = new SwitchBox(tr("Live"));
    m_live->setChecked(true);
    m_live->setToolTip(tr("Keep sampling every %1 s. Unchecked freezes the graphs; the\n"
                          "history already collected is kept.")
                           .arg(PollMs / 1000));
    m_meta = mutedLabel();

    addHeaderWidget(m_live);
    addHeaderWidget(m_meta);

    // --- the scrolling page -----------------------------------------------
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    // Everything on the page either fits the width or scrolls inside its own
    // table; the page itself must never scroll sideways.
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *page = new QWidget;
    auto *col = new QVBoxLayout(page);
    col->setContentsMargins(12, 4, 12, 16);
    col->setSpacing(14);

    auto *cards = new QGridLayout;
    cards->setSpacing(8);
    buildCards(cards);
    col->addLayout(cards);

    auto *charts = new QGridLayout;
    charts->setSpacing(8);
    buildCharts(charts);
    col->addLayout(charts);

    auto *engineRow = new QHBoxLayout;
    engineRow->setSpacing(8);
    auto *engineCol = new QVBoxLayout;
    engineCol->setSpacing(4);
    auto *engineHead = sectionLabel(tr("InnoDB Engine"));
    engineHead->setToolTip(tr("Read from information_schema.INNODB_METRICS — the same counters\n"
                              "SHOW ENGINE INNODB STATUS prints, as typed rows."));
    engineCol->addWidget(engineHead);
    m_engine = makeTable({tr("Metric"), tr("Value")});
    m_engine->setFixedHeight(TableHeight);
    m_engine->setSortingEnabled(false);
    engineCol->addWidget(m_engine);
    engineRow->addLayout(engineCol, 1);

    auto *configCol = new QVBoxLayout;
    configCol->setSpacing(4);
    configCol->addWidget(sectionLabel(tr("Server Configuration")));
    m_config = makeTable({tr("Setting"), tr("Value")});
    m_config->setFixedHeight(TableHeight);
    configCol->addWidget(m_config);
    engineRow->addLayout(configCol, 1);
    col->addLayout(engineRow);

    m_trxLabel = sectionLabel(tr("Live Transactions"));
    col->addWidget(m_trxLabel);
    m_trx = makeTable(
        {tr("Age"), tr("Thread"), tr("State"), tr("Rows Locked"), tr("Rows Modified"),
         tr("Isolation"), tr("Started"), tr("Statement")}
    );
    fitHeight(m_trx, ListHeight);
    col->addWidget(m_trx);

    m_lockLabel = sectionLabel(tr("Lock Waits"));
    col->addWidget(m_lockLabel);
    m_lockWaits = makeTable(
        {tr("Waiting"), tr("Thread"), tr("Blocked By"), tr("Object"), tr("Index"), tr("Lock"),
         tr("Waiting Statement"), tr("Blocking Statement")}
    );
    fitHeight(m_lockWaits, ListHeight);
    col->addWidget(m_lockWaits);

    // The three sections of the monitor text that have no structured source.
    col->addWidget(sectionLabel(tr("Latest Detected Deadlock")));
    m_deadlock = textBox(220);
    col->addWidget(m_deadlock);

    col->addWidget(sectionLabel(tr("Latest Foreign Key Error")));
    m_fkError = textBox(160);
    col->addWidget(m_fkError);

    col->addWidget(sectionLabel(tr("Semaphores")));
    m_semaphores = textBox(130);
    col->addWidget(m_semaphores);

    col->addStretch();
    scroll->setWidget(page);
    setBody(scroll);

    m_timer = new QTimer(this);
    m_timer->setInterval(PollMs);
    connect(
        m_timer, &QTimer::timeout, this,
        [this]()
        {
            if (m_live->isChecked())
            {
                refresh();
            }
        }
    );
    m_timer->start();

    m_sectionTimer = new QTimer(this);
    m_sectionTimer->setInterval(SectionPollMs);
    connect(
        m_sectionTimer, &QTimer::timeout, this,
        [this]()
        {
            if (m_live->isChecked())
            {
                refreshSections();
            }
        }
    );
    m_sectionTimer->start();

    connect(theme::notifier(), &Notifier::changed, this, &DashboardView::applyTheme);
    applyTheme();

    refresh();
    refreshSections();
}

void DashboardView::buildCards(QGridLayout *grid)
{
    struct Spec
    {
        const char *key;
        const char *icon;
        const char *label;
    };

    const Spec specs[] = {
        {"qps", "activity", "Queries / s"},
        {"threads", "cpu", "Threads Running"},
        {"conns", "network", "Connections"},
        {"slow", "clock", "Slow Queries"},
        {"poolhit", "database", "Buffer Pool Hit"},
        {"pooldirty", "database-zap", "Buffer Pool Dirty"},
        {"rows", "table-2", "InnoDB Rows / s"},
        {"io", "gauge", "Disk I/O"},
        {"log", "settings", "Redo Log"},
        {"rowlocks", "eye", "Row Lock Waits"},
        {"deadlocks", "info", "Deadlocks"},
        {"history", "history", "History List"},
    };
    int i = 0;
    for (const Spec &s : specs)
    {
        auto *card = new StatCard(s.icon, tr(s.label));
        m_cards.insert(s.key, card);
        grid->addWidget(card, i / CardColumns, i % CardColumns);
        ++i;
    }
    for (int c = 0; c < CardColumns; ++c)
    {
        grid->setColumnStretch(c, 1);
    }

    m_cards["poolhit"]->setToolTip(
        tr("Reads served from the buffer pool rather than from disk:\n"
           "1 − Innodb_buffer_pool_reads / Innodb_buffer_pool_read_requests.\n"
           "Anything under ~99% on a warm server means the pool is too small.")
    );
    m_cards["history"]->setToolTip(
        tr("InnoDB's undo history length (trx_rseg_history_len). It grows while a\n"
           "long-running transaction holds a read view and blocks purge, which is\n"
           "what makes a busy server's tables bloat.")
    );
    m_cards["deadlocks"]->setToolTip(
        tr("Deadlocks since startup (lock_deadlocks), with lock wait timeouts\n"
           "beside them. The last deadlock's detail is at the bottom of this page.")
    );
}

void DashboardView::buildCharts(QGridLayout *grid)
{
    const AppPalette &pal = theme::current();

    struct Spec
    {
        const char *key;
        const char *title;
        TimeChart::Format format;
        double ceiling;
        QVector<QPair<QString, QColor>> series;
    };

    const QVector<Spec> specs{
        {"queries",
         "Queries / s",
         TimeChart::Format::Number,
         0,
         {{tr("select"), pal.info}, {tr("write"), pal.warning}, {tr("other"), pal.special}}},
        {"threads",
         "Threads",
         TimeChart::Format::Number,
         0,
         {{tr("connected"), pal.info}, {tr("running"), pal.warning}}},
        {"rows",
         "InnoDB Rows / s",
         TimeChart::Format::Number,
         0,
         {{tr("read"), pal.info},
          {tr("inserted"), pal.success},
          {tr("updated"), pal.warning},
          {tr("deleted"), pal.destructive}}},
        {"pool",
         "Buffer Pool",
         TimeChart::Format::Percent,
         100,
         {{tr("hit rate"), pal.success}, {tr("dirty"), pal.warning}}},
        {"io",
         "Disk I/O / s",
         TimeChart::Format::Number,
         0,
         {{tr("reads"), pal.info}, {tr("writes"), pal.warning}, {tr("fsyncs"), pal.special}}},
        {"net",
         "Network",
         TimeChart::Format::Bytes,
         0,
         {{tr("received"), pal.info}, {tr("sent"), pal.success}}},
        {"locks",
         "Lock Waits / s",
         TimeChart::Format::Number,
         0,
         {{tr("row"), pal.destructive}, {tr("table"), pal.warning}, {tr("waiting now"), pal.special}
         }},
        {"tmp",
         "Temp Tables / s",
         TimeChart::Format::Number,
         0,
         {{tr("in memory"), pal.info}, {tr("on disk"), pal.destructive}}},
    };

    int i = 0;
    for (const Spec &s : specs)
    {
        auto *chart = new TimeChart(tr(s.title));
        chart->setFormat(s.format);
        if (s.ceiling > 0)
        {
            chart->setCeiling(s.ceiling);
        }
        for (const auto &series : s.series)
        {
            chart->addSeries(series.first, series.second);
        }
        chart->setSubtitle(tr("last %1 min").arg(TimeChart::Capacity * PollMs / 60000));
        m_charts.insert(s.key, chart);
        grid->addWidget(chart, i / ChartColumns, i % ChartColumns);
        ++i;
    }
    for (int c = 0; c < ChartColumns; ++c)
    {
        grid->setColumnStretch(c, 1);
    }
}

qint64 DashboardView::st(const QString &key) const
{
    return qint64(m_status.value(key).toDouble());
}

qint64 DashboardView::im(const QString &key) const
{
    return qint64(m_innodb.value(key).toDouble());
}

double DashboardView::rate(const QString &key) const
{
    if (m_dt <= 0 || !m_prevStatus.contains(key) || !m_status.contains(key))
    {
        return 0;
    }
    const double delta = m_status.value(key).toDouble() - m_prevStatus.value(key).toDouble();
    // A restart resets the counters; report nothing rather than a huge dip.
    return delta < 0 ? 0 : delta / m_dt;
}

double DashboardView::poolHitRate() const
{
    const double requests = double(st("Innodb_buffer_pool_read_requests"));
    const double reads = double(st("Innodb_buffer_pool_reads"));
    if (requests + reads <= 0)
    {
        return 0;
    }
    return 100.0 * (1.0 - reads / (requests + reads));
}

void DashboardView::refresh()
{
    // One request at a time: a stalled backend must not accumulate a queue
    // whose bunched replies then land milliseconds apart and turn the rate
    // windows into 100x spikes burned into the chart history.
    if (m_inFlight)
    {
        return;
    }
    m_inFlight = true;
    api()->call(
        "admin", "Dashboard", {m_connID}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            m_inFlight = false;
            if (!err.isEmpty())
            {
                showError(err);
                return;
            }
            applySnapshot(res.toObject());
        }
    );
}

void DashboardView::refreshSections()
{
    if (m_sectionsInFlight)
    {
        return;
    }
    m_sectionsInFlight = true;
    api()->call(
        "admin", "InnoDBSections", {m_connID}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            m_sectionsInFlight = false;
            if (!err.isEmpty())
            {
                // The counters above are still live; say why the text is
                // missing without blanking the page. All three boxes carry it,
                // so none is left showing a stale section from a past poll.
                for (QPlainTextEdit *box : {m_deadlock, m_fkError, m_semaphores})
                {
                    box->setPlainText(err);
                    box->setFixedHeight(34);
                }
                return;
            }
            QHash<QString, QString> bodies;
            for (const auto &v : res.toArray())
            {
                const QJsonObject o = v.toObject();
                bodies.insert(o.value("title").toString(), o.value("body").toString());
            }
            auto fill = [&bodies](QPlainTextEdit *box, const QString &title, const QString &empty)
            {
                const QString body = bodies.value(title);
                // A server with no deadlock since startup prints no
                // such section: say so instead of showing an empty box.
                box->setPlainText(body.isEmpty() ? empty : body);
                box->setFixedHeight(body.isEmpty() ? 34 : box->property("fullHeight").toInt());
            };
            fill(
                m_deadlock, kDeadlockSection, tr("No deadlock detected since the server started.")
            );
            fill(
                m_fkError, kForeignKeySection, tr("No foreign key error since the server started.")
            );
            fill(m_semaphores, kSemaphoreSection, tr("Not reported by this server."));
        }
    );
}

void DashboardView::applySnapshot(const QJsonObject &snap)
{
    const QJsonArray notes = snap.value("notes").toArray();
    QStringList noteText;
    for (const auto &n : notes)
    {
        noteText << n.toString();
    }
    showError(noteText.join(" · "));

    m_prevStatus = m_status;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_dt = m_prevAt > 0 ? double(now - m_prevAt) / 1000.0 : 0;
    m_prevAt = now;

    m_status = snap.value("status").toObject();
    m_innodb = snap.value("innodb").toObject();
    m_vars = snap.value("vars").toObject();

    const AppPalette &pal = theme::current();
    m_meta->setText(tr("%1 · up %2 · sampled %3")
                        .arg(
                            snap.value("version").toString(),
                            fmtUptime(qint64(snap.value("uptime").toDouble())),
                            snap.value("sampledAt").toString()
                        ));

    // ---- Cards -----------------------------------------------------------
    const double qps = rate("Questions");
    m_cards["qps"]->setValue(fmtCompact(qps), tr("%1 total").arg(fmtCount(st("Questions"))));

    const qint64 connected = st("Threads_connected");
    const qint64 running = st("Threads_running");
    m_cards["threads"]->setValue(
        fmtCount(running),
        tr("%1 connected · %2 cached").arg(fmtCount(connected), fmtCount(st("Threads_cached")))
    );

    const qint64 maxConns = m_vars.value("max_connections").toString().toLongLong();
    const double used = percentOf(double(connected), double(maxConns));
    m_cards["conns"]->setValue(
        fmtPercent(used),
        tr("%1 of %2 · peak %3")
            .arg(fmtCount(connected), fmtCount(maxConns), fmtCount(st("Max_used_connections")))
    );
    m_cards["conns"]->setTone(used > 90 ? pal.destructive : (used > 75 ? pal.warning : QColor()));

    const qint64 slow = st("Slow_queries");
    m_cards["slow"]->setValue(
        fmtCount(slow),
        tr("%1 / s · over %2 s")
            .arg(
                fmtRate(rate("Slow_queries")), trimZeros(m_vars.value("long_query_time").toString())
            )
    );
    m_cards["slow"]->setTone(rate("Slow_queries") > 1 ? pal.warning : QColor());

    const double hit = poolHitRate();
    m_cards["poolhit"]->setValue(
        fmtPercent(hit),
        tr("%1 pool · %2 reads from disk")
            .arg(
                fmtBytes(m_vars.value("innodb_buffer_pool_size").toString().toLongLong()),
                fmtCount(st("Innodb_buffer_pool_reads"))
            )
    );
    m_cards["poolhit"]->setTone(hit > 0 && hit < 95 ? pal.warning : QColor());

    const double poolTotal = double(st("Innodb_buffer_pool_pages_total"));
    const double dirty = percentOf(double(st("Innodb_buffer_pool_pages_dirty")), poolTotal);
    m_cards["pooldirty"]->setValue(
        fmtPercent(dirty),
        tr("%1 data · %2 free")
            .arg(
                fmtPercent(percentOf(double(st("Innodb_buffer_pool_pages_data")), poolTotal)),
                fmtPercent(percentOf(double(st("Innodb_buffer_pool_pages_free")), poolTotal))
            )
    );

    const double rowsRead = rate("Innodb_rows_read");
    const double rowsWritten =
        rate("Innodb_rows_inserted") + rate("Innodb_rows_updated") + rate("Innodb_rows_deleted");
    m_cards["rows"]->setValue(
        fmtCompact(rowsRead + rowsWritten),
        tr("%1 read · %2 written").arg(fmtCompact(rowsRead), fmtCompact(rowsWritten))
    );

    const double dataReads = rate("Innodb_data_reads");
    const double dataWrites = rate("Innodb_data_writes");
    m_cards["io"]->setValue(
        tr("%1 IOPS").arg(fmtCompact(dataReads + dataWrites)),
        tr("%1 in · %2 out · %3 fsync/s")
            .arg(
                fmtBytesRate(rate("Innodb_data_read")), fmtBytesRate(rate("Innodb_data_written")),
                fmtRate(rate("Innodb_data_fsyncs"))
            )
    );

    const qint64 logWaits = st("Innodb_log_waits");
    m_cards["log"]->setValue(
        fmtCompact(rate("Innodb_log_writes")) + tr(" writes/s"),
        tr("%1 · %2 log waits").arg(fmtBytesRate(rate("Innodb_os_log_written")), fmtCount(logWaits))
    );
    m_cards["log"]->setTone(logWaits > 0 ? pal.warning : QColor());

    const qint64 currentWaits = st("Innodb_row_lock_current_waits");
    m_cards["rowlocks"]->setValue(
        fmtCount(st("Innodb_row_lock_waits")),
        tr("%1 waiting now · avg %2 ms")
            .arg(fmtCount(currentWaits), fmtCount(st("Innodb_row_lock_time_avg")))
    );
    m_cards["rowlocks"]->setTone(currentWaits > 0 ? pal.destructive : QColor());

    // MySQL keeps deadlocks in INNODB_METRICS, MariaDB in a status counter.
    const qint64 deadlocks =
        im("lock_deadlocks") > 0 ? im("lock_deadlocks") : st("Innodb_deadlocks");
    m_cards["deadlocks"]->setValue(
        fmtCount(deadlocks), tr("%1 lock timeouts").arg(fmtCount(im("lock_timeouts")))
    );
    m_cards["deadlocks"]->setTone(deadlocks > 0 ? pal.warning : QColor());

    const qint64 history = im("trx_rseg_history_len") > 0 ? im("trx_rseg_history_len")
                                                          : st("Innodb_history_list_length");
    const int trxCount = int(snap.value("trx").toArray().size());
    m_cards["history"]->setValue(
        fmtCount(history), tr("%1 live transactions").arg(fmtCount(trxCount))
    );
    m_cards["history"]->setTone(history > 100000 ? pal.warning : QColor());

    // ---- Charts ----------------------------------------------------------
    if (m_dt > 0)
    {
        const double sel = rate("Com_select");
        const double write =
            rate("Com_insert") + rate("Com_update") + rate("Com_delete") + rate("Com_replace");
        m_charts["queries"]->push({sel, write, std::max(0.0, qps - sel - write)});
        m_charts["threads"]->push({double(connected), double(running)});
        m_charts["rows"]->push(
            {rate("Innodb_rows_read"), rate("Innodb_rows_inserted"), rate("Innodb_rows_updated"),
             rate("Innodb_rows_deleted")}
        );
        m_charts["pool"]->push({hit, dirty});
        m_charts["io"]->push({dataReads, dataWrites, rate("Innodb_data_fsyncs")});
        m_charts["net"]->push({rate("Bytes_received"), rate("Bytes_sent")});
        m_charts["locks"]->push(
            {rate("Innodb_row_lock_waits"), rate("Table_locks_waited"), double(currentWaits)}
        );
        m_charts["tmp"]->push(
            {std::max(0.0, rate("Created_tmp_tables") - rate("Created_tmp_disk_tables")),
             rate("Created_tmp_disk_tables")}
        );
    }

    fillEngine();
    fillConfig();
    fillTrx(snap);
    fillLockWaits(snap);
}

void DashboardView::fillEngine()
{
    using Row = QPair<QString, QString>;
    const qint64 pageSize = im("innodb_page_size") > 0 ? im("innodb_page_size") : 16384;
    const QVector<Row> rows{
        {tr("Buffer pool size"),
         fmtBytes(m_vars.value("innodb_buffer_pool_size").toString().toLongLong())},
        {tr("Pages total / data / free / dirty"),
         QStringLiteral("%1 / %2 / %3 / %4")
             .arg(
                 fmtCount(st("Innodb_buffer_pool_pages_total")),
                 fmtCount(st("Innodb_buffer_pool_pages_data")),
                 fmtCount(st("Innodb_buffer_pool_pages_free")),
                 fmtCount(st("Innodb_buffer_pool_pages_dirty"))
             )},
        {tr("Pool hit rate"), fmtPercent(poolHitRate())},
        {tr("Read requests / disk reads"), QStringLiteral("%1 / %2").arg(
                                               fmtCount(st("Innodb_buffer_pool_read_requests")),
                                               fmtCount(st("Innodb_buffer_pool_reads"))
                                           )},
        {tr("Waits for a free page"), fmtCount(im("buffer_pool_wait_free"))},
        {tr("Pages created / read / written"),
         QStringLiteral("%1 / %2 / %3")
             .arg(
                 fmtCount(im("buffer_pages_created")), fmtCount(im("buffer_pages_read")),
                 fmtCount(im("buffer_pages_written"))
             )},
        {tr("Page size"), fmtBytes(pageSize)},
        {tr("Redo log capacity"),
         fmtBytes(
             m_vars.value("innodb_redo_log_capacity").toString().isEmpty()
                 ? m_vars.value("innodb_log_file_size").toString().toLongLong()
                 : m_vars.value("innodb_redo_log_capacity").toString().toLongLong()
         )},
        {tr("Log writes / write requests"),
         QStringLiteral("%1 / %2").arg(
             fmtCount(im("log_writes")), fmtCount(im("log_write_requests"))
         )},
        {tr("Log waits"), fmtCount(st("Innodb_log_waits"))},
        {tr("Log bytes written"), fmtBytes(im("os_log_bytes_written"))},
        {tr("Pending log writes / fsyncs"),
         QStringLiteral("%1 / %2").arg(
             fmtCount(im("os_log_pending_writes")), fmtCount(im("os_log_pending_fsyncs"))
         )},
        {tr("Data reads / writes / fsyncs"),
         QStringLiteral("%1 / %2 / %3")
             .arg(
                 fmtCount(im("os_data_reads")), fmtCount(im("os_data_writes")),
                 fmtCount(im("os_data_fsyncs"))
             )},
        {tr("Doublewrite writes / pages"),
         QStringLiteral("%1 / %2").arg(
             fmtCount(im("innodb_dblwr_writes")), fmtCount(im("innodb_dblwr_pages_written"))
         )},
        {tr("Row lock waits / current"),
         QStringLiteral("%1 / %2").arg(
             fmtCount(st("Innodb_row_lock_waits")), fmtCount(st("Innodb_row_lock_current_waits"))
         )},
        {tr("Row lock time avg / max"),
         tr("%1 ms / %2 ms")
             .arg(fmtCount(im("lock_row_lock_time_avg")), fmtCount(im("lock_row_lock_time_max")))},
        {tr("Deadlocks / lock timeouts"),
         QStringLiteral("%1 / %2").arg(
             fmtCount(im("lock_deadlocks")), fmtCount(im("lock_timeouts"))
         )},
        {tr("Undo history length"), fmtCount(im("trx_rseg_history_len"))},
        {tr("Rows read / inserted / updated / deleted"),
         QStringLiteral("%1 / %2 / %3 / %4")
             .arg(
                 fmtCount(st("Innodb_rows_read")), fmtCount(st("Innodb_rows_inserted")),
                 fmtCount(st("Innodb_rows_updated")), fmtCount(st("Innodb_rows_deleted"))
             )},
        {tr("Change buffer size / merges"),
         QStringLiteral("%1 / %2").arg(fmtCount(im("ibuf_size")), fmtCount(im("ibuf_merges")))},
        {tr("Adaptive hash searches (hash / btree)"),
         QStringLiteral("%1 / %2").arg(
             fmtCount(im("adaptive_hash_searches")), fmtCount(im("adaptive_hash_searches_btree"))
         )},
        {tr("Open files"), fmtCount(im("file_num_open_files"))},
        {tr("Table open cache hits / misses / overflows"),
         QStringLiteral("%1 / %2 / %3")
             .arg(
                 fmtCount(st("Table_open_cache_hits")), fmtCount(st("Table_open_cache_misses")),
                 fmtCount(st("Table_open_cache_overflows"))
             )},
        {tr("Temp tables in memory / on disk"),
         QStringLiteral("%1 / %2").arg(
             fmtCount(st("Created_tmp_tables")), fmtCount(st("Created_tmp_disk_tables"))
         )},
        {tr("Full joins / full scans"),
         QStringLiteral("%1 / %2").arg(
             fmtCount(st("Select_full_join")), fmtCount(st("Select_scan"))
         )},
        {tr("Aborted clients / connects"),
         QStringLiteral("%1 / %2").arg(
             fmtCount(st("Aborted_clients")), fmtCount(st("Aborted_connects"))
         )},
    };

    putKV(m_engine, rows);
}

void DashboardView::fillConfig()
{
    QStringList names = m_vars.keys();
    names.sort();
    QVector<QPair<QString, QString>> rows;
    rows.reserve(names.size());
    for (const QString &name : names)
    {
        QString value = m_vars.value(name).toString();
        // Sizes are stored in bytes; show both so a wrong unit is obvious.
        if ((name.endsWith("_size") || name.endsWith("_capacity")) && value.toLongLong() > 1024)
        {
            value = fmtBytes(value.toLongLong()) + " (" + value + ")";
        }
        rows.append({name, value});
    }
    putKV(m_config, rows);
}

void DashboardView::fillTrx(const QJsonObject &snap)
{
    const QJsonArray trx = snap.value("trx").toArray();
    m_trxLabel->setText(
        trx.isEmpty() ? tr("Live Transactions (none)")
                      : tr("Live Transactions (%1)").arg(trx.size())
    );
    const AppPalette &pal = theme::current();
    m_trx->setSortingEnabled(false);
    m_trx->setRowCount(0);
    for (const auto &v : trx)
    {
        const QJsonObject o = v.toObject();
        const int row = m_trx->rowCount();
        m_trx->insertRow(row);
        const qint64 age = qint64(o.value("seconds").toDouble());
        auto *ageItem = numItem(age);
        // A transaction open for minutes is the usual cause of undo bloat and
        // of lock waits elsewhere; make it visible without reading the number.
        if (age > 60)
        {
            ageItem->setForeground(pal.destructive);
        }
        else if (age > 10)
        {
            ageItem->setForeground(pal.warning);
        }
        m_trx->setItem(row, 0, ageItem);
        m_trx->setItem(row, 1, numItem(qint64(o.value("thread").toDouble())));
        m_trx->setItem(row, 2, new QTableWidgetItem(o.value("state").toString()));
        m_trx->setItem(row, 3, numItem(qint64(o.value("rowsLocked").toDouble())));
        m_trx->setItem(row, 4, numItem(qint64(o.value("rowsModified").toDouble())));
        m_trx->setItem(row, 5, new QTableWidgetItem(o.value("isolation").toString()));
        m_trx->setItem(row, 6, new QTableWidgetItem(o.value("started").toString()));
        const QString q = o.value("query").toString();
        auto *stmt = new QTableWidgetItem(q.simplified());
        stmt->setToolTip(q);
        m_trx->setItem(row, 7, stmt);
    }
    m_trx->setSortingEnabled(true);
    fitColumns(m_trx);
    fitHeight(m_trx, ListHeight);
}

void DashboardView::fillLockWaits(const QJsonObject &snap)
{
    const QJsonArray waits = snap.value("lockWaits").toArray();
    m_lockLabel->setText(
        waits.isEmpty() ? tr("Lock Waits (none)") : tr("Lock Waits (%1)").arg(waits.size())
    );
    m_lockWaits->setSortingEnabled(false);
    m_lockWaits->setRowCount(0);
    for (const auto &v : waits)
    {
        const QJsonObject o = v.toObject();
        const int row = m_lockWaits->rowCount();
        m_lockWaits->insertRow(row);
        m_lockWaits->setItem(row, 0, numItem(qint64(o.value("seconds").toDouble())));
        m_lockWaits->setItem(row, 1, numItem(qint64(o.value("waitingThread").toDouble())));
        m_lockWaits->setItem(row, 2, numItem(qint64(o.value("blockingThread").toDouble())));
        m_lockWaits->setItem(
            row, 3,
            new QTableWidgetItem(o.value("schema").toString() + "." + o.value("table").toString())
        );
        m_lockWaits->setItem(row, 4, new QTableWidgetItem(o.value("index").toString()));
        m_lockWaits->setItem(
            row, 5,
            new QTableWidgetItem(
                o.value("lockType").toString() + " " + o.value("lockMode").toString()
            )
        );
        auto *waiting = new QTableWidgetItem(o.value("waitingQuery").toString().simplified());
        waiting->setToolTip(o.value("waitingQuery").toString());
        m_lockWaits->setItem(row, 6, waiting);
        auto *blocking = new QTableWidgetItem(o.value("blockingQuery").toString().simplified());
        blocking->setToolTip(o.value("blockingQuery").toString());
        m_lockWaits->setItem(row, 7, blocking);
    }
    m_lockWaits->setSortingEnabled(true);
    fitColumns(m_lockWaits);
    fitHeight(m_lockWaits, ListHeight);
}

void DashboardView::applyTheme()
{
    const AppPalette &pal = theme::current();
    for (StatCard *c : m_cards)
    {
        c->applyTheme();
    }
    const QVector<QVector<QColor>> colours{
        {pal.info, pal.warning, pal.special},
        {pal.info, pal.warning},
        {pal.info, pal.success, pal.warning, pal.destructive},
        {pal.success, pal.warning},
        {pal.info, pal.warning, pal.special},
        {pal.info, pal.success},
        {pal.destructive, pal.warning, pal.special},
        {pal.info, pal.destructive},
    };
    const QStringList order{"queries", "threads", "rows", "pool", "io", "net", "locks", "tmp"};
    for (int i = 0; i < order.size(); ++i)
    {
        m_charts[order.at(i)]->setSeriesColours(colours.at(i));
    }
    m_error->setStyleSheet(
        QString("QLabel { color: %1; padding: 4px 12px; }").arg(pal.warning.name())
    );
}
