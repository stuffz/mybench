#include "shell/mainwindow.h"

#include "app/api.h"
#include "app/icons.h"
#include "app/theme.h"
#include "dialogs/aboutdialog.h"
#include "dialogs/connectionsdialog.h"
#include "dialogs/importrowsdialog.h"
#include "dialogs/prefsdialog.h"
#include "dialogs/shortcutsdialog.h"
#include "editor/editortab.h"
#include "editor/resultgrid.h"
#include "shell/servertabbar.h"
#include "shell/sidebar.h"
#include "shell/statusstrip.h"
#include "shell/tabs.h"
#include "shell/workspace.h"
#include "ui/closeglyph.h"
#include "ui/widgets.h"
#include "views/dashboardview.h"
#include "views/graphview.h"
#include "views/historyview.h"
#include "views/innodbview.h"
#include "views/inspectorview.h"
#include "views/processlistview.h"
#include "views/serverinfoview.h"
#include "views/usersview.h"

#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <functional>

namespace
{

constexpr int SaveCoalesceMs = 50;
// How long a footer confirmation or sidebar error stays up.
constexpr int MessageMs = 8000;
// Header height as a multiple of the base font size (39px at the default
// 13px), not a flat px value: a fixed height crowded the controls on scaled
// monitors, where everything inside the header grows but the bar would not.
constexpr double HeaderScale = 3.0;
// Tab close button: icon at the base font size, hit area a touch wider.
constexpr double CloseIconScale = 1.0;
constexpr double CloseButtonScale = 1.4;
// Space between the × glyph and the tab's right divider, carried inside the
// button (see styleTabCloseButton). Unscaled like the qss tab paddings.
constexpr int CloseEdgeGapPx = 6;

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle("mybench");
    {
        const QSettings settings;
        const QByteArray geometry = settings.value("window/geometry").toByteArray();
        // restoreGeometry validates against the current screen layout, so a
        // window saved on a monitor that is gone still lands somewhere usable.
        if (geometry.isEmpty() || !restoreGeometry(geometry))
        {
            resize(1280, 800);
        }
    }

    const AppPalette &pal = theme::current();

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // --- header -----------------------------------------------------------
    auto *header = new QWidget;
    m_header = header;
    header->setObjectName("Header");
    header->setFixedHeight(theme::scaledPx(HeaderScale));
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(12, 0, 12, 0);
    hl->setSpacing(12);
    // The same lucide database glyph the web header uses, drawn rather than
    // shipped as an asset: a cylinder outline with three bands.
    m_headerIcon = new QLabel;
    m_headerIcon->setPixmap(icons::pixmap("database", pal.foreground, 16));
    auto *brand = weightedLabel("mybench", QFont::DemiBold);
    // Icon and wordmark read as one unit, so they sit closer together (6px)
    // than the 12px the header puts between its groups — as on the web.
    auto *brandWrap = new QWidget;
    auto *bl = new QHBoxLayout(brandWrap);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(6);
    bl->addWidget(m_headerIcon);
    bl->addWidget(brand);
    hl->addWidget(brandWrap);
    auto *connBtn = new QPushButton(tr("Connections"));
    connect(connBtn, &QPushButton::clicked, this, &MainWindow::openConnectionsDialog);
    hl->addWidget(connBtn);
    hl->addStretch();
    m_prefsBtn = new QPushButton(tr("Preferences"));
    m_prefsBtn->setProperty("variant", "ghost");
    m_prefsBtn->setIcon(icons::icon("settings", pal.mutedFg, 13));
    connect(m_prefsBtn, &QPushButton::clicked, this, &MainWindow::openPreferences);
    hl->addWidget(m_prefsBtn);
    m_shortcutsBtn = new QPushButton(tr("Shortcuts"));
    m_shortcutsBtn->setProperty("variant", "ghost");
    m_shortcutsBtn->setIcon(icons::icon("keyboard", pal.mutedFg, 13));
    connect(m_shortcutsBtn, &QPushButton::clicked, this, &MainWindow::openShortcuts);
    hl->addWidget(m_shortcutsBtn);
    m_aboutBtn = new QPushButton(tr("About"));
    m_aboutBtn->setProperty("variant", "ghost");
    m_aboutBtn->setIcon(icons::icon("info", pal.mutedFg, 13));
    connect(m_aboutBtn, &QPushButton::clicked, this, &MainWindow::openAbout);
    hl->addWidget(m_aboutBtn);
    root->addWidget(header);

    // --- server tabs ------------------------------------------------------
    m_serverTabs = new ServerTabBar;
    connect(m_serverTabs, &ServerTabBar::activated, this, &MainWindow::setActiveConnection);
    connect(m_serverTabs, &ServerTabBar::closeRequested, this, &MainWindow::closeConnection);
    root->addWidget(m_serverTabs);

    // --- sidebar | tabs ---------------------------------------------------
    m_sidebar = new Sidebar;
    m_sidebar->setMinimumWidth(160);
    m_sidebar->setMaximumWidth(560);
    connect(
        m_sidebar, &Sidebar::tabRequested, this,
        [this](const TabRequest &r)
        {
            if (!m_activeConn.isEmpty())
            {
                addTab(m_activeConn, r);
            }
        }
    );
    connect(
        m_sidebar, &Sidebar::graphFocusRequested, this,
        [this](const QString &schema, const QString &table)
        {
            if (m_activeConn.isEmpty())
            {
                return;
            }
            m_graphFocus.insert(m_activeConn, {schema, table});
            TabRequest r;
            r.view = TabView::Graph;
            addTab(m_activeConn, r);
        }
    );
    connect(
        m_sidebar, &Sidebar::importRowsRequested, this,
        [this](const QString &schema, const QString &table)
        {
            if (m_activeConn.isEmpty())
            {
                return;
            }
            ImportRowsDialog dlg(m_activeConn, schema, table, this);
            if (dlg.exec() == QDialog::Accepted)
            {
                m_status->showMessage(
                    tr("%L1 rows inserted into %2.%3").arg(dlg.insertedCount()).arg(schema, table),
                    MessageMs
                );
            }
        }
    );
    connect(
        m_sidebar, &Sidebar::errorRaised, this,
        [this](const QString &msg) { m_status->showMessage(msg, MessageMs); }
    );

    m_emptyState = new QWidget;
    {
        auto *lay = new QVBoxLayout(m_emptyState);
        lay->setAlignment(Qt::AlignCenter);
        // Same lucide database-zap the web empty state used, dimmed.
        auto *glyph = new QLabel;
        glyph->setObjectName("emptyGlyph");
        QColor dim = theme::current().mutedFg;
        dim.setAlphaF(0.45);
        glyph->setPixmap(icons::pixmap("database-zap", dim, 32));
        auto *t1 = new QLabel(tr("No open connections"));
        t1->setAlignment(Qt::AlignCenter);
        t1->setProperty("muted", true);
        auto *t2 = new QLabel(tr("Use \"Connections\" in the header to connect to a server."));
        t2->setAlignment(Qt::AlignCenter);
        t2->setProperty("muted", true);
        lay->addWidget(glyph, 0, Qt::AlignHCenter);
        lay->addWidget(t1, 0, Qt::AlignHCenter);
        lay->addWidget(t2, 0, Qt::AlignHCenter);
    }

    m_panesStack = new QStackedWidget;
    m_panesStack->addWidget(m_emptyState);

    m_split = new QSplitter(Qt::Horizontal);
    m_split->addWidget(m_sidebar);
    m_split->addWidget(m_panesStack);
    m_split->setStretchFactor(1, 1);
    m_split->setSizes({240, 1040});
    m_sidebar->setMinimumWidth(220);
    m_sidebar->setVisible(false);
    root->addWidget(m_split, 1);

    // --- status strip -----------------------------------------------------
    m_status = new StatusStrip;
    m_status->setVisible(false);
    root->addWidget(m_status);

    setCentralWidget(central);
    statusBar()->setSizeGripEnabled(false);
    statusBar()->hide();

    connect(theme::notifier(), &Notifier::changed, this, &MainWindow::applyTheme);

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(SaveCoalesceMs);
    connect(m_saveTimer, &QTimer::timeout, this, &MainWindow::saveWorkspace);

    // Coalesced: a drag-resize fires resizeEvent per frame.
    m_geometryTimer = new QTimer(this);
    m_geometryTimer->setSingleShot(true);
    m_geometryTimer->setInterval(600);
    connect(m_geometryTimer, &QTimer::timeout, this, &MainWindow::saveGeometryNow);

    // Tab keyboard control. Qt::CTRL means Command on macOS, so these come
    // out as Cmd+T / Cmd+W there — the platform's own tab keys.
    auto *newTabKey = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_T), this);
    connect(newTabKey, &QShortcut::activated, this, &MainWindow::newQueryTab);
    auto *closeTabKey = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_W), this);
    connect(closeTabKey, &QShortcut::activated, this, &MainWindow::closeActiveTab);
    auto *connectionsKey = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T), this);
    connect(connectionsKey, &QShortcut::activated, this, &MainWindow::openConnectionsDialog);
    auto *quickKey = new QShortcut(quickConnectShortcut(), this);
    connect(quickKey, &QShortcut::activated, this, &MainWindow::openQuickConnect);
    auto *closeConnKey = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W), this);
    connect(
        closeConnKey, &QShortcut::activated, this,
        [this]()
        {
            if (!m_activeConn.isEmpty())
            {
                closeConnection(m_activeConn);
            }
        }
    );

    Q_UNUSED(pal);
}

void MainWindow::saveGeometryNow()
{
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveGeometryNow();
    // Flush the coalesced workspace save: a change made inside the coalesce
    // window would otherwise die with the event loop, and even a fired save
    // needs the loop pumped for its POST to reach the wire before quit.
    if (m_saveTimer->isActive())
    {
        m_saveTimer->stop();
        saveWorkspace();
    }
    api()->flush(1000);
    QMainWindow::closeEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    m_geometryTimer->start();
}

void MainWindow::moveEvent(QMoveEvent *event)
{
    QMainWindow::moveEvent(event);
    m_geometryTimer->start();
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    QWindow *handle = windowHandle();
    if (!handle || m_screenHooked)
    {
        return;
    }
    m_screenHooked = true;
    // Sizes are derived from the logical DPI of the screen the window is on,
    // so it has to be re-pointed and re-applied whenever that changes.
    const auto retarget = [this](QScreen *s)
    {
        if (s)
        {
            // Also covers the scale of the current monitor being changed. A
            // member function, not a lambda: Qt::UniqueConnection — which is
            // what stops a revisited screen being hooked twice — is refused
            // for functors (invalid connection in release, an assert in
            // debug), so with a lambda this hook never existed at all.
            connect(
                s, &QScreen::logicalDotsPerInchChanged, this, &MainWindow::onScreenDpiChanged,
                Qt::UniqueConnection
            );
        }
        retargetTheme(s);
    };
    connect(handle, &QWindow::screenChanged, this, retarget);
    // The first apply happened before any window existed, so it sized against
    // the primary screen; redo it now that the real one is known.
    retarget(handle->screen());
}

void MainWindow::onScreenDpiChanged()
{
    // Hooks accumulate on every screen the window has visited, so a change
    // can arrive from one it is no longer on; sizing against that monitor
    // would be wrong, so only the current screen may retarget.
    auto *s = qobject_cast<QScreen *>(sender());
    if (s && windowHandle() && windowHandle()->screen() == s)
    {
        retargetTheme(s);
    }
}

void MainWindow::retargetTheme(QScreen *screen)
{
    theme::setDpiScreen(screen);
    theme::apply(
        m_prefs.value("appTheme").toString(theme::defaultApp), m_prefs.value("uiFontSize").toInt(13)
    );
}

void MainWindow::applyTheme()
{
    const AppPalette &pal = theme::current();
    // The height is font-derived, and both the slider and a monitor change
    // land here — the constructor's value only covered the startup screen.
    if (m_header)
    {
        m_header->setFixedHeight(theme::scaledPx(HeaderScale));
    }
    m_serverTabs->applyTheme();
    m_status->applyTheme();
    m_sidebar->applyTheme();
    if (m_headerIcon)
    {
        m_headerIcon->setPixmap(icons::pixmap("database", pal.foreground, 16));
    }
    if (auto *glyph = m_emptyState->findChild<QLabel *>("emptyGlyph"))
    {
        QColor dim = pal.mutedFg;
        dim.setAlphaF(0.45);
        glyph->setPixmap(icons::pixmap("database-zap", dim, 32));
    }
    if (m_prefsBtn)
    {
        m_prefsBtn->setIcon(icons::icon("settings", pal.mutedFg, 13));
    }
    if (m_shortcutsBtn)
    {
        m_shortcutsBtn->setIcon(icons::icon("keyboard", pal.mutedFg, 13));
    }
    if (m_aboutBtn)
    {
        m_aboutBtn->setIcon(icons::icon("info", pal.mutedFg, 13));
    }
    // Tab close glyphs live on the tab bars, one per open tab.
    for (QTabWidget *pane : m_panes)
    {
        for (int i = 0; i < pane->count(); ++i)
        {
            styleTabCloseButton(pane, i);
        }
    }
    for (const Tab &t : m_tabs)
    {
        if (auto *e = qobject_cast<EditorTab *>(t.widget))
        {
            e->applyTheme();
        }
    }
}

QColor MainWindow::connColor(const QString &connID) const
{
    for (const QJsonObject &c : m_saved)
    {
        if (c.value("id").toString() != connID)
        {
            continue;
        }
        const QString custom = c.value("color").toString();
        if (!custom.isEmpty())
        {
            return QColor(custom);
        }
        break;
    }
    return theme::connAccent(connID);
}

void MainWindow::onBackendReady()
{
    refreshSaved([this]() { loadWorkspace(); });
}

void MainWindow::refreshSaved(const std::function<void()> &then)
{
    api()->call(
        "conn", "List", {}, this,
        [this, then](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                QMessageBox::warning(this, tr("mybench"), err);
                return;
            }
            m_saved.clear();
            for (const auto &v : res.toArray())
            {
                if (v.isObject())
                {
                    m_saved.append(v.toObject());
                }
            }
            rebuildServerTabs();
            if (then)
            {
                then();
            }
        }
    );
}

void MainWindow::openConnectionsDialog()
{
    execConnectionsDialog(ConnectionsDialog::Start::List);
}

void MainWindow::openQuickConnect()
{
    execConnectionsDialog(ConnectionsDialog::Start::QuickConnect);
}

void MainWindow::execConnectionsDialog(ConnectionsDialog::Start start)
{
    ConnectionsDialog dlg(m_saved, m_openIDs, start, this);
    connect(&dlg, &ConnectionsDialog::connectRequested, this, &MainWindow::openConnection);
    connect(&dlg, &ConnectionsDialog::disconnectRequested, this, &MainWindow::closeConnection);
    // The conn.List reply runs on this window's context and can outlive the
    // dialog; guard the stack-allocated dialog with a QPointer.
    connect(
        &dlg, &ConnectionsDialog::savedChanged, this,
        [this, dlgp = QPointer<ConnectionsDialog>(&dlg)]()
        {
            refreshSaved(
                [this, dlgp]()
                {
                    if (dlgp)
                    {
                        dlgp->setSaved(m_saved, m_openIDs);
                    }
                }
            );
        }
    );
    dlg.exec();
}

void MainWindow::openPreferences()
{
    PrefsDialog dlg(m_prefs, this);
    connect(
        &dlg, &PrefsDialog::prefsChanged, this,
        [this](const QJsonObject &p)
        {
            m_prefs = p;
            theme::apply(
                p.value("appTheme").toString(theme::defaultApp), p.value("uiFontSize").toInt(13)
            );
            const QString editorTheme = p.value("editorTheme").toString(theme::defaultEditor);
            theme::setCurrentEditor(editorTheme);
            ResultGrid::setCopySeparator(p.value("copySeparator").toString("\t"));
            m_sidebar->setHideDefaultDBs(p.value("hideDefaultDBs").toBool(true));
            for (const Tab &t : m_tabs)
            {
                if (auto *e = qobject_cast<EditorTab *>(t.widget))
                {
                    e->applyEditorPrefs(
                        editorTheme, p.value("editorFontSize").toInt(13),
                        p.value("tabSize").toInt(4)
                    );
                    e->setDefaultRowLimit(p.value("defaultRowLimit").toInt(50000));
                }
            }
            m_saveTimer->start();
        }
    );
    dlg.exec();
}

void MainWindow::openShortcuts()
{
    showShortcutsDialog(this);
}

void MainWindow::openAbout()
{
    showAboutDialog(this);
}

void MainWindow::openConnection(const QString &connID)
{
    api()->call(
        "conn", "Open", {connID}, this,
        [this, connID](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                QMessageBox::warning(this, tr("Connect"), err);
                return;
            }
            const QJsonObject st = res.toObject();
            if (!st.value("error").toString().isEmpty())
            {
                QMessageBox::warning(this, tr("Connect"), st.value("error").toString());
                return;
            }
            if (!m_openIDs.contains(connID))
            {
                m_openIDs.append(connID);
            }
            setActiveConnection(connID);
            // A restored workspace may already carry this connection's tabs.
            bool has = false;
            for (const Tab &t : m_tabs)
            {
                has = has || t.connID == connID;
            }
            if (!has)
            {
                addTab(connID, TabRequest{});
            }
            else
            {
                // Materialise the restored tabs now that the connection is live.
                QTabWidget *pane = paneFor(connID);
                for (Tab &t : m_tabs)
                {
                    if (t.connID != connID || t.widget)
                    {
                        continue;
                    }
                    t.widget = buildTabWidget(t);
                    styleTabCloseButton(pane, pane->addTab(t.widget, t.title));
                }
                const QString wanted = m_activePerConn.value(connID);
                for (int i = 0; i < m_tabs.size(); ++i)
                {
                    if (m_tabs.at(i).connID == connID && m_tabs.at(i).tabID == wanted)
                    {
                        pane->setCurrentWidget(m_tabs.at(i).widget);
                    }
                }
                // The pane only exists now, so re-activate to bring it on screen —
                // the earlier call had nothing to switch to.
                setActiveConnection(connID);
            }
            rebuildServerTabs();
            m_saveTimer->start();
        }
    );
}

void MainWindow::closeConnection(const QString &connID)
{
    api()->post("conn", "Close", {connID});
    m_openIDs.removeAll(connID);
    // Tabs go dormant, not away: editor content syncs back into the Tab
    // entry (the widget dies with the pane) and openConnection() re-
    // materialises them — disconnect/reconnect keeps a connection's tabs,
    // the same way an app restart does.
    for (Tab &t : m_tabs)
    {
        if (t.connID != connID)
        {
            continue;
        }
        if (auto *e = qobject_cast<EditorTab *>(t.widget))
        {
            t.sql = e->sql();
            t.editorH = e->editorHeight();
        }
        t.widget = nullptr;
    }
    if (QTabWidget *pane = m_panes.take(connID))
    {
        m_panesStack->removeWidget(pane);
        pane->deleteLater();
    }
    if (m_activeConn == connID)
    {
        setActiveConnection(m_openIDs.isEmpty() ? QString() : m_openIDs.first());
    }
    rebuildServerTabs();
    m_saveTimer->start();
}

void MainWindow::setActiveConnection(const QString &connID)
{
    m_activeConn = connID;
    if (connID.isEmpty())
    {
        // Clear the sidebar's identity too: without this, reconnecting the
        // same server looks like a no-op to it and the stale tree survives.
        m_sidebar->setConnection({}, false);
        m_sidebar->setVisible(false);
        m_status->watch({}, {});
        m_panesStack->setCurrentWidget(m_emptyState);
        rebuildServerTabs();
        return;
    }
    m_sidebar->setVisible(true);
    m_sidebar->setHideDefaultDBs(m_prefs.value("hideDefaultDBs").toBool(true));
    m_sidebar->setConnection(connID, m_openIDs.contains(connID));
    QString label = connID;
    for (const QJsonObject &c : m_saved)
    {
        if (c.value("id").toString() == connID)
        {
            label = QString("%1 — %2@%3:%4")
                        .arg(
                            c.value("name").toString(), c.value("user").toString(),
                            c.value("host").toString()
                        )
                        .arg(c.value("port").toInt());
            break;
        }
    }
    m_status->watch(connID, label);
    if (QTabWidget *pane = m_panes.value(connID))
    {
        m_panesStack->setCurrentWidget(pane);
    }
    rebuildServerTabs();
    m_saveTimer->start();
}

void MainWindow::rebuildServerTabs()
{
    QVector<QPair<QString, QString>> conns;
    QHash<QString, QColor> colors;
    for (const QString &id : m_openIDs)
    {
        QString name = id;
        for (const QJsonObject &c : m_saved)
        {
            if (c.value("id").toString() == id)
            {
                name = c.value("name").toString();
            }
        }
        conns.append({id, name});
        colors.insert(id, connColor(id));
    }
    m_serverTabs->setConnections(conns, colors, m_activeConn);
}

QTabWidget *MainWindow::paneFor(const QString &connID)
{
    if (QTabWidget *p = m_panes.value(connID))
    {
        return p;
    }
    auto *pane = new QTabWidget;
    // Named for the narrower right padding in theme.cpp: the close button's
    // hit area is wider than its glyph, and that transparent margin already
    // pads the tab's right side.
    pane->tabBar()->setObjectName("QueryTabs");
    // Closing is the custom glyph's job (styleTabCloseButton), which every
    // tab gets. Not tabsClosable: that adds a stock button on the side the
    // style dictates — right on Linux and Windows, where setTabButton then
    // replaces it, but LEFT on macOS, where it survived beside the glyph as
    // a second close icon.
    pane->setMovable(true);
    // No document mode: its paintEvent skips the stylesheet pane frame, whose
    // top border is the full-width separator under the tab strip.
    connect(
        pane, &QTabWidget::currentChanged, this,
        [this, connID, pane](int index)
        {
            QWidget *w = pane->widget(index);
            for (const Tab &t : m_tabs)
            {
                if (t.widget == w)
                {
                    m_activePerConn.insert(connID, t.tabID);
                }
            }
            m_saveTimer->start();
        }
    );
    pane->setProperty("connID", connID);

    // Right-click menu: on a tab via its bar, and on the pane itself so the
    // empty strip right of the tabs — or a pane with no tabs at all — still
    // offers "New Query Tab".
    pane->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
        pane->tabBar(), &QTabBar::customContextMenuRequested, this, [this, pane](const QPoint &pos)
        { showTabContextMenu(pane, pane->tabBar()->tabAt(pos), pane->tabBar()->mapToGlobal(pos)); }
    );
    pane->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(
        pane, &QTabWidget::customContextMenuRequested, this,
        [this, pane](const QPoint &pos)
        {
            // Unhandled right-clicks inside a page bubble up here too; only
            // the strip itself (or a pane with no tabs at all) is ours.
            if (pane->count() > 0 && pos.y() > pane->tabBar()->geometry().bottom())
            {
                return;
            }
            showTabContextMenu(pane, -1, pane->mapToGlobal(pos));
        }
    );
    connect(
        pane, &QTabWidget::tabBarDoubleClicked, this,
        [this, pane](int index) { renameTab(pane, index); }
    );
    pane->tabBar()->installEventFilter(this); // middle-click close

    m_panes.insert(connID, pane);
    m_panesStack->addWidget(pane);
    return pane;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease)
    {
        auto *bar = qobject_cast<QTabBar *>(watched);
        auto *me = static_cast<QMouseEvent *>(event);
        if (bar && me->button() == Qt::MiddleButton)
        {
            const int at = bar->tabAt(me->position().toPoint());
            auto *pane = qobject_cast<QTabWidget *>(bar->parentWidget());
            if (at >= 0 && pane)
            {
                closeTab(pane->property("connID").toString(), at);
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

Tab *MainWindow::tabForWidget(QWidget *w)
{
    for (Tab &t : m_tabs)
    {
        if (t.widget == w)
        {
            return &t;
        }
    }
    return nullptr;
}

void MainWindow::newQueryTab()
{
    if (!m_activeConn.isEmpty())
    {
        addTab(m_activeConn, TabRequest{});
    }
}

void MainWindow::closeActiveTab()
{
    QTabWidget *pane = m_panes.value(m_activeConn);
    if (pane && pane->currentIndex() >= 0)
    {
        closeTab(m_activeConn, pane->currentIndex());
    }
}

void MainWindow::showTabContextMenu(QTabWidget *pane, int index, const QPoint &globalPos)
{
    const QString connID = pane->property("connID").toString();
    QMenu menu(this);

    QAction *newTab = menu.addAction(
        tr("New Query Tab"), this, [this, connID]() { addTab(connID, TabRequest{}); }
    );
    newTab->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));
    newTab->setShortcutVisibleInContextMenu(true);

    if (index >= 0)
    {
        const Tab *t = tabForWidget(pane->widget(index));
        // Panels exist once per connection — "duplicating" one would only
        // reactivate it — and their titles already say what they are, so
        // duplicate is for editors and inspectors, rename for editors only.
        if (t && !isSingletonView(t->view))
        {
            menu.addAction(
                tr("Duplicate Tab"), this, [this, connID, index]() { duplicateTab(connID, index); }
            );
        }
        if (t && t->view == TabView::Editor)
        {
            menu.addSeparator();
            menu.addAction(
                tr("Rename Tab…"), this, [this, pane, index]() { renameTab(pane, index); }
            );
        }

        menu.addSeparator();
        QAction *close = menu.addAction(
            tr("Close Tab"), this, [this, connID, index]() { closeTab(connID, index); }
        );
        close->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_W));
        close->setShortcutVisibleInContextMenu(true);

        // Closing goes widget-by-widget: indices shift as tabs go, widgets
        // stay put.
        const auto closeWidgets = [this, pane, connID](const QList<QWidget *> &targets)
        {
            for (QWidget *w : targets)
            {
                const int at = pane->indexOf(w);
                if (at >= 0)
                {
                    closeTab(connID, at);
                }
            }
        };
        const auto range = [pane](int from, int to)
        {
            QList<QWidget *> out;
            for (int i = from; i < to; ++i)
            {
                out.append(pane->widget(i));
            }
            return out;
        };

        QAction *others = menu.addAction(
            tr("Close Other Tabs"), this, [closeWidgets, range, pane, index]()
            { closeWidgets(range(0, index) + range(index + 1, pane->count())); }
        );
        others->setEnabled(pane->count() > 1);
        QAction *left = menu.addAction(
            tr("Close Tabs to the Left"), this,
            [closeWidgets, range, index]() { closeWidgets(range(0, index)); }
        );
        left->setEnabled(index > 0);
        QAction *right = menu.addAction(
            tr("Close Tabs to the Right"), this,
            [closeWidgets, range, pane, index]() { closeWidgets(range(index + 1, pane->count())); }
        );
        right->setEnabled(index < pane->count() - 1);
        menu.addAction(
            tr("Close All Tabs"), this,
            [closeWidgets, range, pane]() { closeWidgets(range(0, pane->count())); }
        );
    }
    menu.exec(globalPos);
}

void MainWindow::duplicateTab(const QString &connID, int index)
{
    QTabWidget *pane = m_panes.value(connID);
    const Tab *t = pane ? tabForWidget(pane->widget(index)) : nullptr;
    if (!t || isSingletonView(t->view))
    {
        return;
    }
    TabRequest req;
    req.view = t->view;
    req.schema = t->schema;
    req.table = t->table;
    req.section = t->section;
    // The live editor buffer, not the last debounced sync.
    auto *e = qobject_cast<EditorTab *>(t->widget);
    req.sql = e ? e->sql() : t->sql;
    addTab(connID, req); // invalidates t: m_tabs may reallocate
}

void MainWindow::renameTab(QTabWidget *pane, int index)
{
    Tab *t = index >= 0 ? tabForWidget(pane->widget(index)) : nullptr;
    if (!t || t->view != TabView::Editor)
    {
        return; // panel titles say what the panel is; only query tabs rename
    }
    bool ok = false;
    const QString name =
        QInputDialog::getText(
            this, tr("Rename Tab"), tr("Tab Name"), QLineEdit::Normal, t->title, &ok
        )
            .trimmed();
    if (!ok || name.isEmpty() || name == t->title)
    {
        return;
    }
    t->title = name;
    t->titled = true;
    pane->setTabText(index, name);
    m_saveTimer->start();
}

// Qt's stock close icon ignores the palette; a flat glyph button matches the
// web tab strip and follows the theme.
void MainWindow::styleTabCloseButton(QTabWidget *pane, int index)
{
    const AppPalette &pal = theme::current();
    auto *btn = new QPushButton;
    const int iconPx = theme::scaledPx(CloseIconScale);
    btn->setIcon(icons::icon("x", pal.mutedFg, iconPx));
    // Without this the button's default 16px icon ceiling would shrink the
    // glyph back down on scaled monitors.
    btn->setIconSize(QSize(iconPx, iconPx));
    // The style pins this button a fixed ~4px from the tab's right edge and
    // ignores the tab's padding, so the breathing room between the glyph and
    // the divider has to live inside the button: extra width on the right,
    // with padding keeping the glyph in the left portion. Unscaled, to match
    // the stylesheet's unscaled tab paddings.
    const int side = theme::scaledPx(CloseButtonScale);
    btn->setFixedSize(side + CloseEdgeGapPx, side);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setToolTip(tr("Close Tab"));
    btn->setStyleSheet(QString("QPushButton { border: none; background: transparent; color: %1; "
                               "padding: 0 %4px 0 0; font-size: %3px; }"
                               "QPushButton:hover { color: %2; }")
                           .arg(pal.mutedFg.name(), pal.foreground.name())
                           .arg(theme::scaledPx(CloseGlyphScale))
                           .arg(CloseEdgeGapPx));
    QWidget *page = pane->widget(index);
    connect(
        btn, &QPushButton::clicked, this,
        [this, pane, page]()
        {
            const int at = pane->indexOf(page);
            if (at >= 0)
            {
                closeTab(pane->property("connID").toString(), at);
            }
        }
    );
    pane->tabBar()->setTabButton(index, QTabBar::RightSide, btn);
}

QString MainWindow::tabTitle(const QString &connID, const TabRequest &req) const
{
    switch (req.view)
    {
    case TabView::Editor:
    {
        // Number query tabs per server; reuse of closed numbers is fine.
        int n = 1;
        for (const Tab &t : m_tabs)
        {
            if (t.connID == connID && t.view == TabView::Editor)
            {
                ++n;
            }
        }
        return tr("Query %1").arg(n);
    }
    case TabView::Dashboard:
        return tr("Dashboard");
    case TabView::Processlist:
        return tr("Client Connections");
    case TabView::Users:
        return tr("Users");
    case TabView::ServerInfo:
        return tr("Server Info");
    case TabView::InnoDB:
        return tr("InnoDB Status");
    case TabView::Graph:
        return tr("Schema Graph");
    case TabView::History:
        return tr("Query History");
    case TabView::TableInspect:
        return req.schema + "." + req.table;
    case TabView::SchemaInspect:
        return req.schema + tr(" (schema)");
    }
    return tr("Tab");
}

QWidget *MainWindow::buildTabWidget(Tab &tab)
{
    switch (tab.view)
    {
    case TabView::Editor:
    {
        auto *e = new EditorTab(tab.connID, tab.tabID, tab.sql);
        const QString tabID = tab.tabID;
        connect(
            e, &EditorTab::sqlChanged, this,
            [this, tabID](const QString &sql)
            {
                for (Tab &t : m_tabs)
                {
                    if (t.tabID == tabID)
                    {
                        t.sql = sql;
                    }
                }
                m_saveTimer->start();
            }
        );
        e->setConnected(m_openIDs.contains(tab.connID));
        e->applyEditorPrefs(
            m_prefs.value("editorTheme").toString(theme::defaultEditor),
            m_prefs.value("editorFontSize").toInt(13), m_prefs.value("tabSize").toInt(4)
        );
        e->setDefaultRowLimit(m_prefs.value("defaultRowLimit").toInt(50000));
        if (tab.editorH > 0)
        {
            e->setEditorHeight(tab.editorH);
        }
        return e;
    }
    case TabView::Dashboard:
        return new DashboardView(tab.connID);
    case TabView::Processlist:
        return new ProcesslistView(tab.connID);
    case TabView::Users:
        return new UsersView(tab.connID);
    case TabView::ServerInfo:
        return new ServerInfoView(tab.connID);
    case TabView::InnoDB:
        return new InnoDBView(tab.connID);
    case TabView::History:
        return new HistoryView(tab.connID);
    case TabView::Graph:
    {
        auto *g = new GraphView(tab.connID);
        const auto focus = m_graphFocus.value(tab.connID);
        if (!focus.second.isEmpty())
        {
            g->focusOn(focus.first, focus.second);
        }
        return g;
    }
    case TabView::TableInspect:
    case TabView::SchemaInspect:
        return new InspectorView(
            tab.connID, tab.schema, tab.view == TabView::TableInspect ? tab.table : QString(),
            tab.section
        );
    }
    return new QWidget;
}

void MainWindow::addTab(const QString &connID, const TabRequest &req)
{
    QTabWidget *pane = paneFor(connID);

    if (isSingletonView(req.view))
    {
        for (const Tab &t : m_tabs)
        {
            if (t.connID != connID || t.view != req.view || !t.widget)
            {
                continue;
            }
            pane->setCurrentWidget(t.widget);
            setActiveConnection(connID);
            // "Show in Graph" on an already-open graph still needs to focus.
            if (req.view == TabView::Graph)
            {
                const auto focus = m_graphFocus.value(connID);
                if (auto *g = qobject_cast<GraphView *>(t.widget); g && !focus.second.isEmpty())
                {
                    g->focusOn(focus.first, focus.second);
                }
            }
            return;
        }
    }

    Tab tab;
    tab.tabID = "t" + QString::number(++m_tabSeq);
    tab.connID = connID;
    tab.view = req.view;
    tab.schema = req.schema;
    tab.table = req.table;
    tab.section = req.section;
    tab.sql = req.sql;
    tab.title = tabTitle(connID, req);
    m_tabs.append(tab);
    Tab &stored = m_tabs.last();
    stored.widget = buildTabWidget(stored);
    const int ix = pane->addTab(stored.widget, stored.title);
    styleTabCloseButton(pane, ix);
    pane->setCurrentWidget(stored.widget);
    m_activePerConn.insert(connID, stored.tabID);
    setActiveConnection(connID);
    m_saveTimer->start();
}

void MainWindow::closeTab(const QString &connID, int index)
{
    QTabWidget *pane = m_panes.value(connID);
    if (!pane)
    {
        return;
    }
    QWidget *w = pane->widget(index);
    pane->removeTab(index);
    for (int i = 0; i < m_tabs.size(); ++i)
    {
        if (m_tabs.at(i).widget != w)
        {
            continue;
        }
        m_tabs.removeAt(i);
        break;
    }
    w->deleteLater();
    // A pane never sits empty: closing the last tab (one Ctrl+W too many,
    // "Close All Tabs") leaves a fresh query tab instead of a blank pane.
    if (pane->count() == 0)
    {
        addTab(connID, TabRequest{});
    }
    m_saveTimer->start();
}

// ------------------------------------------------------------ workspace ---

void MainWindow::saveWorkspace()
{
    if (!m_restored)
    {
        return; // never let an early save wipe the stored workspace
    }
    const int sidebarWidth =
        m_sidebar->isVisible() && m_split->sizes().value(0) > 0 ? m_split->sizes().value(0) : -1;
    const QJsonObject ws =
        encodeWorkspace(m_tabs, m_activeConn, m_activePerConn, m_prefs, sidebarWidth);
    api()->post(
        "workspace", "Save", {QString::fromUtf8(QJsonDocument(ws).toJson(QJsonDocument::Compact))}
    );
}

void MainWindow::loadWorkspace()
{
    api()->call(
        "workspace", "Load", {}, this,
        [this](const QJsonValue &res, const QString &err)
        {
            if (!err.isEmpty())
            {
                return; // keep m_restored false: suppressing saves beats wiping the store
            }
            m_restored = true;
            const QJsonDocument doc = QJsonDocument::fromJson(res.toString().toUtf8());
            if (!doc.isObject())
            {
                return;
            }

            QSet<QString> known;
            for (const QJsonObject &c : m_saved)
            {
                known.insert(c.value("id").toString());
            }
            const WorkspaceState st = decodeWorkspace(doc.object(), known);

            m_prefs = st.prefs;
            theme::apply(
                m_prefs.value("appTheme").toString(theme::defaultApp),
                m_prefs.value("uiFontSize").toInt(13)
            );
            theme::setCurrentEditor(m_prefs.value("editorTheme").toString(theme::defaultEditor));
            ResultGrid::setCopySeparator(m_prefs.value("copySeparator").toString("\t"));
            // Clamp: a stale collapsed width would hide the panel labels.
            const int sw = qBound(220, st.sidebarWidth, 560);
            m_split->setSizes({sw, qMax(400, width() - sw)});

            // Restore the tab tree but dial nothing: connections open only when
            // the user asks (SPEC: session restore is lazy). Tabs materialise in
            // openConnection(). Titles are derived here, one tab at a time, so
            // editor tabs number themselves by arrival like live ones do —
            // except titles the user typed, which restore verbatim.
            m_tabSeq = qMax(m_tabSeq, st.maxTabSeq);
            for (const Tab &decoded : st.tabs)
            {
                Tab t = decoded;
                if (!t.titled || t.title.isEmpty())
                {
                    TabRequest req;
                    req.view = t.view;
                    req.schema = t.schema;
                    req.table = t.table;
                    t.title = tabTitle(t.connID, req);
                }
                m_tabs.append(t);
            }
            m_activePerConn = st.activePerConn;
        }
    );
}
