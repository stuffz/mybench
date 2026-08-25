#pragma once
// SHOW ENGINE INNODB STATUS, verbatim. The report is meant to be read as the
// engine prints it, so it stays unwrapped text rather than a parsed table.
#include "views/panelbase.h"

class QPlainTextEdit;

class InnoDBView final : public PanelBase
{
    Q_OBJECT
public:
    explicit InnoDBView(const QString &connID, QWidget *parent = nullptr);

protected:
    void refresh() override;

private:
    QPlainTextEdit *m_text;
};
