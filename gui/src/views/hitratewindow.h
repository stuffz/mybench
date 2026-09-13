#pragma once
// A rolling window over the two buffer pool counters.
//
// Innodb_buffer_pool_read_requests and Innodb_buffer_pool_reads are monotonic
// since the server started, so their ratio is a lifetime average: on a server
// up for 86 days with 12.5 trillion requests banked, a pool that begins
// thrashing right now would take over a day of total cache failure to move the
// displayed figure by a single percent. This window divides the deltas instead,
// so the card answers for the last minute rather than the last quarter.
#include <QList>

#include <optional>

class HitRateWindow
{
public:
    explicit HitRateWindow(qint64 windowMs);

    void push(qint64 at, double requests, double reads);

    // The percentage of requests served from the pool across the window, or
    // nothing at all when there is no read activity to divide: fewer than two
    // samples, an idle server, or counters that went backwards over a restart.
    // Those are three different kinds of "no answer", and none of them is 0%.
    std::optional<double> ratio() const;

    void clear();

private:
    struct Sample
    {
        qint64 at = 0;
        double requests = 0;
        double reads = 0;
    };

    qint64 m_windowMs;
    QList<Sample> m_samples;
};
