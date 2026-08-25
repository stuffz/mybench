#pragma once
// Every number the panels print: counts, rates, percentages, byte sizes and
// uptimes, in one place so two panels never spell the same value differently.
#include <QString>

QString fmtCount(qint64 n);
QString fmtRate(double v);

// Busy counters, short: 5.4M rather than 5,384,778.
QString fmtCompact(double v);

QString fmtPercent(double v);

// Clamped to 0–100, and 0 when whole is not positive: a missing denominator
// should read as nothing rather than as a spike.
double percentOf(double part, double whole);

QString fmtBytes(qint64 n);

// Bytes per second, for the I/O and network lines.
QString fmtBytesRate(double v);

// Coarse by design — the largest two units and no more.
QString fmtUptime(qint64 seconds);

// Server values like long_query_time arrive as "10.000000".
QString trimZeros(const QString &v);
