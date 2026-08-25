#pragma once
// The shell: header bar, one server tab per open connection, the active
// server's sidebar + query/panel tabs, and the status strip. Tabs stay alive
// while their connection is open so editor and result state survive switching,
// and the whole tab tree round-trips through WorkspaceService.
#include <QHash>
#include <QJsonObject>
#include <QMainWindow>
#include <QVector>
#include <functional>

#include "dialogs/connectionsdialog.h"
#include "shell/tabs.h"

class QLabel;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTabWidget;
class QTimer;
class ServerTabBar;
class Sidebar;
class StatusStrip;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    void onBackendReady();

protected:
    // Window size and position are remembered here rather than in the
    // workspace blob: that blob is shared with the wails build, which rewrites
    // it without unknown keys and would drop them.
    void closeEvent(QCloseEvent *event) override;
    // Saved on change as well as on close, so a kill or a logout does not lose
    // it — closeEvent alone only covers an orderly quit.
    void resizeEvent(QResizeEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    // Dragging the window to a monitor with a different scale changes the
    // logical DPI, and the px sizes are derived from it — so the theme has to
    // be re-applied there. The hook needs a window handle, which only exists
    // from the first show.
    void showEvent(QShowEvent *event) override;
    // Middle-click on a query tab bar closes that tab.
    bool eventFilter(QObject *watched, QEvent *event) override;

public slots:
    void openConnectionsDialog();
    void openQuickConnect();
    void openPreferences();
    void openShortcuts();
    void openAbout();

private:
    void refreshSaved(const std::function<void()> &then = {});
    void execConnectionsDialog(ConnectionsDialog::Start start);
    void openConnection(const QString &connID);
    void closeConnection(const QString &connID);
    void setActiveConnection(const QString &connID);
    void addTab(const QString &connID, const TabRequest &req);
    void closeTab(const QString &connID, int index);
    void newQueryTab();
    void closeActiveTab();
    void showTabContextMenu(QTabWidget *pane, int index, const QPoint &globalPos);
    void duplicateTab(const QString &connID, int index);
    void renameTab(QTabWidget *pane, int index);
    Tab *tabForWidget(QWidget *w);
    QWidget *buildTabWidget(Tab &tab);
    void styleTabCloseButton(QTabWidget *pane, int index);
    QTabWidget *paneFor(const QString &connID);
    void rebuildServerTabs();
    void saveWorkspace();
    void saveGeometryNow();
    void loadWorkspace();
    QString tabTitle(const QString &connID, const TabRequest &req) const;
    QColor connColor(const QString &connID) const;

    // Saved profiles, mirrored from ConnectionService.List.
    QVector<QJsonObject> m_saved;
    QVector<QString> m_openIDs;
    QVector<Tab> m_tabs;
    QString m_activeConn;
    QHash<QString, QString> m_activePerConn; // connID → tabID
    QHash<QString, QTabWidget *> m_panes;
    QHash<QString, QPair<QString, QString>> m_graphFocus; // connID → schema/table
    int m_tabSeq = 0;
    bool m_restored = false;
    bool m_screenHooked = false; // showEvent can fire more than once
    QJsonObject m_prefs;

    void applyTheme();
    // Points the theme at `screen`'s logical DPI and re-applies it.
    void retargetTheme(class QScreen *screen);
    // The screen the window is on changed its DPI/scale; retarget.
    void onScreenDpiChanged();

    QWidget *m_header = nullptr;
    QLabel *m_headerIcon = nullptr;
    QPushButton *m_prefsBtn = nullptr;
    QPushButton *m_shortcutsBtn = nullptr;
    QPushButton *m_aboutBtn = nullptr;
    ServerTabBar *m_serverTabs;
    Sidebar *m_sidebar;
    QSplitter *m_split;
    QStackedWidget *m_panesStack;
    QWidget *m_emptyState;
    StatusStrip *m_status;
    QTimer *m_saveTimer;
    QTimer *m_geometryTimer;
};
