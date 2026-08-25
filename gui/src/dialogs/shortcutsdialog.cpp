#include "dialogs/shortcutsdialog.h"

#include "app/icons.h"
#include "app/theme.h"
#include "dialogs/connectionsdialog.h"
#include "ui/widgets.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QKeySequence>
#include <QLabel>
#include <QVBoxLayout>
#include <QVector>

namespace
{

struct Row
{
    QString keys; // already in native text ("Ctrl+T" here, "⌘T" on macOS)
    QString what;
};

struct Group
{
    QString title;
    QVector<Row> rows;
};

QString native(const QKeySequence &seq)
{
    return seq.toString(QKeySequence::NativeText);
}

} // namespace

void showShortcutsDialog(QWidget *parent)
{
    const AppPalette &pal = theme::current();

    // Kept by hand, next to the bindings they document — a registry would be
    // more machinery than the app has shortcuts.
    const QVector<Group> groups{
        {QObject::tr("Application"),
         {
             {native(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T)),
              QObject::tr("Open the Connections dialog")},
             {native(quickConnectShortcut()), QObject::tr("Quick Connect to a Teleport database")},
             {native(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W)),
              QObject::tr("Close the current server tab")},
         }},
        {QObject::tr("Query Tabs"),
         {
             {native(QKeySequence(Qt::CTRL | Qt::Key_T)), QObject::tr("New query tab")},
             {native(QKeySequence(Qt::CTRL | Qt::Key_W)), QObject::tr("Close the current tab")},
         }},
        {QObject::tr("Editor"),
         {
             {native(QKeySequence(Qt::CTRL | Qt::Key_Return)),
              QObject::tr("Run the statement at the cursor")},
             {native(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Return)),
              QObject::tr("Run the whole script")},
             {native(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F)),
              QObject::tr("Format the selection or the whole buffer")},
             {native(QKeySequence(Qt::CTRL | Qt::Key_Space)),
              QObject::tr("Completion — Enter or Tab accepts, Esc dismisses")},
         }},
        {QObject::tr("Results"),
         {
             {native(QKeySequence(QKeySequence::Copy)),
              QObject::tr("Copy the selected cells (Copy Separator preference)")},
         }},
        {QObject::tr("Mouse"),
         {
             {QObject::tr("Middle-click a tab"), QObject::tr("Close it")},
             {QObject::tr("Double-click a query tab"), QObject::tr("Rename it")},
             {QObject::tr("Right-click the tab bar"),
              QObject::tr("Tab actions — new, duplicate, rename, close")},
             {QObject::tr("Right-click a result cell"),
              QObject::tr("View the full value, copy the selection, copy rows")},
             {QObject::tr("Double-click a result cell"), QObject::tr("Edit it in place")},
         }},
    };

    QDialog dlg(parent);
    dlg.setWindowTitle(QObject::tr("Keyboard Shortcuts"));
    auto *lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(20, 20, 20, 16);
    lay->setSpacing(10);

    auto *head = new QHBoxLayout;
    head->setSpacing(8);
    auto *glyph = new QLabel;
    glyph->setPixmap(icons::pixmap("keyboard", pal.foreground, theme::scaledPx(1.4)));
    head->addWidget(glyph);
    auto *title = new QLabel(QObject::tr("Keyboard Shortcuts"));
    title->setObjectName("kpiValue"); // 1.5× base, semibold — same as About
    head->addWidget(title);
    head->addStretch();
    lay->addLayout(head);

    for (const Group &g : groups)
    {
        lay->addWidget(hairline());
        lay->addWidget(mutedLabel(g.title));
        auto *grid = new QGridLayout;
        grid->setHorizontalSpacing(18);
        grid->setVerticalSpacing(4);
        int r = 0;
        for (const Row &row : g.rows)
        {
            auto *keys = weightedLabel(row.keys, QFont::DemiBold);
            grid->addWidget(keys, r, 0);
            grid->addWidget(new QLabel(row.what), r, 1);
            ++r;
        }
        grid->setColumnStretch(1, 1);
        lay->addLayout(grid);
    }
    lay->addSpacing(4);

    auto *box = new QDialogButtonBox;
    box->addButton(QObject::tr("Close"), QDialogButtonBox::AcceptRole);
    QObject::connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    lay->addWidget(box);

    // Sized to content, not pinned like About: nothing here word-wraps, so
    // the natural size hint fits the longest row at any font scale, where a
    // pinned width clipped whichever description outgrew it.
    dlg.adjustSize();
    dlg.exec();
}
