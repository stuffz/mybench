#include "dialogs/aboutdialog.h"

#include "app/api.h"
#include "app/icons.h"
#include "app/theme.h"
#include "ui/widgets.h"

#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QVBoxLayout>
#include <QVector>
#include <array>

namespace
{

// RFC3339 → "yyyy-MM-dd hh:mm" in local time; the raw string when unparsable.
QString fmtBuildDate(const QString &iso)
{
    const QDateTime d = QDateTime::fromString(iso, Qt::ISODate);
    return d.isValid() ? d.toLocalTime().toString("yyyy-MM-dd HH:mm") : iso;
}

} // namespace

void showAboutDialog(QWidget *parent)
{
    api()->call(
        "admin", "AppInfo", {}, parent,
        [parent](const QJsonValue &res, const QString &err)
        {
            const AppPalette &pal = theme::current();
            const QJsonObject o = res.toObject();

            QDialog dlg(parent);
            dlg.setWindowTitle(QObject::tr("About mybench"));
            auto *lay = new QVBoxLayout(&dlg);
            lay->setContentsMargins(20, 20, 20, 16);
            lay->setSpacing(10);

            auto *head = new QHBoxLayout;
            head->setSpacing(8);
            auto *glyph = new QLabel;
            glyph->setPixmap(icons::pixmap("database", pal.foreground, theme::scaledPx(1.4)));
            head->addWidget(glyph);
            auto *title = new QLabel("mybench");
            // Sized by the sheet (#kpiValue: 1.5× base, semibold) — a
            // setFont here would be overridden by the global rule.
            title->setObjectName("kpiValue");
            head->addWidget(title);
            head->addStretch();
            lay->addLayout(head);

            auto *tag = mutedLabel(
                QObject::tr("A fast, minimal MySQL GUI — one keyring-backed connection store, "
                            "per-tab sessions, and result grids that survive millions of rows.")
            );
            tag->setWordWrap(true);
            lay->addWidget(tag);

            auto *build = smallLabel();
            if (err.isEmpty())
            {
                // Dev builds carry no version/date stamps; show only what is
                // actually set instead of "version · build dev".
                QStringList parts;
                if (const QString v = o.value("version").toString(); !v.isEmpty())
                {
                    parts << QObject::tr("version %1").arg(v);
                }
                if (const QString c = o.value("commit").toString(); !c.isEmpty())
                {
                    parts << QObject::tr("build %1").arg(c);
                }
                if (const QString d = o.value("date").toString(); !d.isEmpty())
                {
                    parts << fmtBuildDate(d);
                }
                build->setText(parts.join(QStringLiteral(" · ")));
            }
            else
            {
                build->setText(err);
            }
            build->setVisible(!build->text().isEmpty());
            lay->addWidget(build);

            lay->addWidget(hairline());

            auto *builtOn = mutedLabel(QObject::tr("Built on:"));
            lay->addWidget(builtOn);

            // What the native client actually ships; the permissive
            // licenses don't require in-app attribution, but the
            // projects deserve it (the web build credits its own).
            const QVector<std::array<QString, 3>> credits{
                {"Qt 6", "https://www.qt.io", QObject::tr("GUI toolkit (LGPLv3)")},
                {"Go", "https://go.dev", QObject::tr("Backend")},
                {"go-sql-driver/mysql", "https://github.com/go-sql-driver/mysql",
                 QObject::tr("pure-Go MySQL Driver")},
                {"Lucide", "https://lucide.dev", QObject::tr("Icons (ISC)")},
                {"RobotoMono Nerd Font", "https://www.nerdfonts.com", QObject::tr("Typeface")},
            };
            // Note right after each name, like the web list — a column-aligned
            // grid left a gulf behind the short names.
            auto *rows = new QVBoxLayout;
            rows->setSpacing(4);
            for (const auto &credit : credits)
            {
                auto *row = new QHBoxLayout;
                row->setSpacing(10);
                auto *link = new QLabel(QString("<a href=\"%1\" style=\"color:%2;\">%3</a>")
                                            .arg(credit[1], pal.foreground.name(), credit[0]));
                link->setTextFormat(Qt::RichText);
                link->setOpenExternalLinks(true);
                row->addWidget(link);
                auto *note = mutedLabel(credit[2]);
                row->addWidget(note);
                row->addStretch();
                rows->addLayout(row);
            }
            lay->addLayout(rows);
            lay->addSpacing(4);

            auto *box = new QDialogButtonBox;
            auto *close = box->addButton(QObject::tr("Close"), QDialogButtonBox::AcceptRole);
            Q_UNUSED(close);
            QObject::connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
            lay->addWidget(box);

            dlg.setFixedWidth(theme::scaledPx(34.0));
            // Refit to the pinned width before showing, the way prefsdialog and
            // connectionsdialog do. The height the dialog is born with was
            // computed before setFixedWidth applied, and the wrapped tagline
            // above needs a height that depends on the width — so on a scaled
            // display it opened short and the credit rows came out clipped.
            lay->activate();
            dlg.adjustSize();
            dlg.exec();
        }
    );
}
