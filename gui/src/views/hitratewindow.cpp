#include "views/hitratewindow.h"

#include <optional>

HitRateWindow::HitRateWindow(qint64 windowMs) : m_windowMs(windowMs) {}

void HitRateWindow::push(qint64 at, double requests, double reads)
{
    m_samples.append({at, requests, reads});
    // The newest sample always survives, so a poll that lands after a long gap
    // leaves something for the next one to divide against.
    while (m_samples.size() > 1 && at - m_samples.first().at > m_windowMs)
    {
        m_samples.removeFirst();
    }
}

std::optional<double> HitRateWindow::ratio() const
{
    if (m_samples.size() < 2)
    {
        return std::nullopt;
    }
    const Sample &oldest = m_samples.first();
    const Sample &newest = m_samples.last();
    const double requests = newest.requests - oldest.requests;
    const double reads = newest.reads - oldest.reads;
    if (requests <= 0 || reads < 0)
    {
        return std::nullopt;
    }
    return 100.0 * (1.0 - reads / requests);
}

void HitRateWindow::clear()
{
    m_samples.clear();
}
