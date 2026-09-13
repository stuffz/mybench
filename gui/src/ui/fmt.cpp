#include "ui/fmt.h"

#include <QLocale>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace
{

// Under ten a single decimal still carries information; above it the integer
// reads cleaner. Callers that need the tail ask for decimals explicitly.
constexpr double PercentOneDecimalBelow = 10.0;

} // namespace

QString fmtCount(qint64 n)
{
    return QLocale().toString(n);
}

QString fmtRate(double v)
{
    const QLocale loc;
    if (v > 0 && v < 10)
    {
        return loc.toString(v, 'f', 1);
    }
    return loc.toString(qint64(std::llround(v)));
}

QString fmtCompact(double v)
{
    const QLocale loc;
    if (v >= 1e9)
    {
        return loc.toString(v / 1e9, 'f', 1) + "G";
    }
    if (v >= 1e6)
    {
        return loc.toString(v / 1e6, 'f', 1) + "M";
    }
    if (v >= 10000)
    {
        return loc.toString(v / 1e3, 'f', 0) + "k";
    }
    return fmtRate(v);
}

QString fmtPercent(double v)
{
    return fmtPercent(v, v < PercentOneDecimalBelow ? 1 : 0);
}

QString fmtPercent(double v, int decimals)
{
    return QLocale().toString(v, 'f', decimals) + "%";
}

double percentOf(double part, double whole)
{
    return whole > 0 ? std::clamp(100.0 * part / whole, 0.0, 100.0) : 0.0;
}

QString fmtBytes(qint64 n)
{
    if (n < 1024)
    {
        return QString("%1 B").arg(n);
    }
    static const QStringList units{"KiB", "MiB", "GiB", "TiB"};
    double v = double(n) / 1024.0;
    int u = 0;
    while (v >= 1024 && u < units.size() - 1)
    {
        v /= 1024;
        ++u;
    }
    return QString("%1 %2").arg(v, 0, 'f', v < 10 ? 1 : 0).arg(units.at(u));
}

QString fmtBytesRate(double v)
{
    return fmtBytes(qint64(std::llround(v))) + "/s";
}

QString fmtUptime(qint64 seconds)
{
    const qint64 d = seconds / 86400;
    const qint64 h = (seconds % 86400) / 3600;
    const qint64 m = (seconds % 3600) / 60;
    if (d > 0)
    {
        return QStringLiteral("%1d %2h").arg(d).arg(h);
    }
    if (h > 0)
    {
        return QStringLiteral("%1h %2m").arg(h).arg(m);
    }
    return QStringLiteral("%1m").arg(m);
}

QString trimZeros(const QString &v)
{
    if (!v.contains('.'))
    {
        return v;
    }
    QString out = v;
    while (out.endsWith('0'))
    {
        out.chop(1);
    }
    if (out.endsWith('.'))
    {
        out.chop(1);
    }
    return out;
}
