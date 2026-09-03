#include "dialogs/querydialog.h"

#include "app/api.h"
#include "app/theme.h"
#include "editor/sqlhighlighter.h"
#include "ui/widgets.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace
{

constexpr int FormatTabWidth = 4;

} // namespace

void showQueryDialog(QWidget *parent, const QString &title, const QString &sql)
{
    const EditorPalette &ep = theme::currentEditor();
    const AppPalette &pal = theme::current();

    QDialog dlg(parent);
    dlg.setWindowTitle(title);
    auto *lay = new QVBoxLayout(&dlg);

    auto *body = new QPlainTextEdit(sql);
    body->setReadOnly(true);
    body->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    // The editor palette and the mono face, sized like the UI: the app
    // stylesheet's global font rule would otherwise override setFont, and the
    // editor slider's size is not reachable from here.
    body->setStyleSheet(
        QString("QPlainTextEdit { background: %1; color: %2; border: 1px solid %3; "
                "border-radius: 6px; padding: 4px; font-family: \"%4\"; font-size: %5px; "
                "selection-background-color: %6; selection-color: %2; }")
            .arg(ep.bg.name(), ep.fg.name(), pal.input.name(), theme::monoFamily())
            .arg(theme::uiFontSize())
            .arg(ep.sel.name())
    );
    new SqlHighlighter(body->document());
    body->setMinimumSize(theme::scaledPx(44.0), theme::scaledPx(20.0));
    lay->addWidget(body);

    auto *error = smallLabel();
    error->setStyleSheet(QString("QLabel { color: %1; }").arg(pal.destructive.name()));
    error->setWordWrap(true);
    error->hide();
    lay->addWidget(error);

    auto *box = new QDialogButtonBox;
    auto *format = box->addButton(QObject::tr("Format"), QDialogButtonBox::ActionRole);
    auto *copy = box->addButton(QObject::tr("Copy"), QDialogButtonBox::ActionRole);
    box->addButton(QDialogButtonBox::Close);
    QObject::connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(
        copy, &QPushButton::clicked, body,
        [body]() { QApplication::clipboard()->setText(body->toPlainText()); }
    );
    QObject::connect(
        format, &QPushButton::clicked, body,
        [body, format, error]()
        {
            format->setEnabled(false);
            api()->call(
                "sqlfmt", "Format", {body->toPlainText(), FormatTabWidth}, body,
                [body, format, error](const QJsonValue &res, const QString &err)
                {
                    if (!err.isEmpty())
                    {
                        error->setText(QObject::tr("format: %1").arg(err));
                        error->show();
                        format->setEnabled(true);
                        return;
                    }
                    body->setPlainText(res.toString());
                }
            );
        }
    );
    lay->addWidget(box);

    dlg.exec();
}
