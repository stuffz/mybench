// Native mybench client. The window comes up immediately; the Go backend is
// spawned alongside and the UI populates when it reports its port.
#include "app/appstyle.h"
#include "app/backend.h"
#include "app/singleinstance.h"
#include "app/theme.h"
#include "shell/mainwindow.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCommandLineParser>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QTextStream>
#include <QTimer>
#include <QWidget>

namespace
{

// Prints the live widget tree: class, object name, geometry, and whatever text
// the widget carries. Screenshots answer "does it look right"; this answers
// "is that field 90px or 400px wide, and do these two labels share a column",
// which is what layout bugs actually turn on.
// NOLINTNEXTLINE(misc-no-recursion) — a tree walk; depth is the widget tree's
void dumpTree(const QWidget *w, QTextStream &out, int depth = 0)
{
    QString text;
    if (const auto *l = qobject_cast<const QLabel *>(w))
    {
        text = l->text();
    }
    else if (const auto *b = qobject_cast<const QAbstractButton *>(w))
    {
        text = b->text();
    }
    else if (const auto *e = qobject_cast<const QLineEdit *>(w))
    {
        text = e->text().isEmpty() ? "[" + e->placeholderText() + "]" : e->text();
    }

    const QRect g = w->geometry();
    out << QString(qsizetype(depth) * 2, ' ') << w->metaObject()->className();
    if (!w->objectName().isEmpty())
    {
        out << '#' << w->objectName();
    }
    out << QString(" [%1,%2 %3x%4]").arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height());
    if (!w->isVisible())
    {
        out << " (hidden)";
    }
    if (!text.isEmpty())
    {
        out << "  \"" << text.simplified() << '"';
    }
    out << '\n';

    for (const QObject *child : w->children())
    {
        if (const auto *cw = qobject_cast<const QWidget *>(child))
        {
            dumpTree(cw, out, depth + 1);
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName("mybench");
    app.setOrganizationName("mybench");

    // --screenshot exists so a headless build can prove the window renders;
    // it grabs the window after --delay ms and exits.
    QCommandLineParser parser;
    QCommandLineOption shot("screenshot", "Write a PNG of the window and exit.", "path");
    QCommandLineOption delay("delay", "Milliseconds to wait before the grab.", "ms", "2500");
    QCommandLineOption dump("dump-ui", "Print the widget tree to stdout and exit.");
    parser.addOption(shot);
    parser.addOption(delay);
    parser.addOption(dump);
    parser.addHelpOption();
    parser.process(app);

    // The headless flags exist to check a build while mybench may already be
    // open, so they must not hand off to that window.
    SingleInstance instance(SingleInstance::defaultDir());
    const bool headless = parser.isSet(shot) || parser.isSet(dump);
    if (!headless)
    {
        switch (instance.claim())
        {
        case SingleInstance::Role::Primary:
            break;
        case SingleInstance::Role::Unguarded:
            QMessageBox::warning(
                nullptr, QObject::tr("mybench"),
                QObject::tr("Another launch will open a second window instead of "
                            "raising this one:\n%1")
                    .arg(instance.errorString())
            );
            break;
        case SingleInstance::Role::Forwarded:
            return 0;
        case SingleInstance::Role::Unreachable:
            QMessageBox::critical(
                nullptr, QObject::tr("mybench"),
                QObject::tr("mybench is already running but not responding.")
            );
            return 1;
        }
    }

    app.setStyle(new AppStyle); // QApplication takes ownership

    theme::loadFonts();
    theme::apply(theme::defaultApp, 13);

    MainWindow w;
    QObject::connect(
        &instance, &SingleInstance::activationRequested, &w,
        [&w]()
        {
            w.setWindowState(w.windowState() & ~Qt::WindowMinimized);
            w.show();
            w.raise();
            w.activateWindow();
        }
    );

    auto *backend = new Backend(&app);
    QObject::connect(backend, &Backend::ready, &w, &MainWindow::onBackendReady);
    QObject::connect(
        backend, &Backend::failed, &w,
        [&w](const QString &why) { QMessageBox::critical(&w, QObject::tr("mybench"), why); }
    );
    QObject::connect(&app, &QCoreApplication::aboutToQuit, backend, &Backend::stop);
    backend->start();

    w.show();

    if (parser.isSet(dump))
    {
        const int ms = parser.value(delay).toInt();
        QTimer::singleShot(
            ms, &w,
            [&w]()
            {
                QTextStream out(stdout);
                for (QWidget *top : QApplication::topLevelWidgets())
                {
                    if (top->isVisible() || top == &w)
                    {
                        dumpTree(top, out);
                    }
                }
                out.flush();
                qApp->quit();
            }
        );
    }

    if (parser.isSet(shot))
    {
        const int ms = parser.value(delay).toInt();
        QTimer::singleShot(
            ms, &w,
            [&w, &parser, &shot]()
            {
                w.grab().save(parser.value(shot));
                qApp->quit();
            }
        );
    }

    return app.exec();
}
