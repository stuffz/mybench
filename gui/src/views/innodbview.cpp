#include "views/innodbview.h"

#include "app/api.h"
#include "views/panelbase.h"

#include <QPlainTextEdit>

InnoDBView::InnoDBView(const QString &connID, QWidget *parent)
    : PanelBase(tr("InnoDB Status"), connID, parent)
{
    m_text = new QPlainTextEdit;
    m_text->setReadOnly(true);
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_text->setFrameShape(QFrame::NoFrame);
    setBody(m_text);
    refresh();
}

void InnoDBView::refresh()
{
    api()->call(
        "admin", "InnoDBStatus", {m_connID}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                showError(err);
                return;
            }
            showError({});
            m_text->setPlainText(res.toString());
        }
    );
}
