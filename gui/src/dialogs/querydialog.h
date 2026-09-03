#pragma once
// A read-only, syntax-coloured view of one SQL statement: what a client
// connection is running, which the processlist column truncates to a line.
#include <QString>

class QWidget;

void showQueryDialog(QWidget *parent, const QString &title, const QString &sql);
