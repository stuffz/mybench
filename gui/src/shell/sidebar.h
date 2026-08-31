#pragma once
// Per-server sidebar: an Administration list and a lazily loaded
// schema → table → column tree with a filter box. Left-click selects and
// expands; everything that acts lives on right-click, same as the web build.
#include <QHash>
#include <QSet>
#include <QWidget>

#include "shell/tabs.h"

class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QListWidget;

class Sidebar : public QWidget
{
    Q_OBJECT
public:
    explicit Sidebar(QWidget *parent = nullptr);

    void setConnection(const QString &connID, bool connected);
    void setHideDefaultDBs(bool on);
    // Re-render the item glyphs and the panel chrome after a theme switch.
    void applyTheme();
    void reload();

signals:
    void tabRequested(const TabRequest &req);
    void graphFocusRequested(const QString &schema, const QString &table);
    void importRowsRequested(const QString &schema, const QString &table);
    void errorRaised(const QString &message);

private:
    enum ItemKind
    {
        SchemaItem,
        TableItem,
        ColumnItem
    };

    void buildAdmin();
    void applyRefreshIcon();
    void loadSchemas();
    void loadTables(QTreeWidgetItem *schemaItem);
    void loadColumns(QTreeWidgetItem *tableItem);
    void applyFilter();
    void showMenu(const QPoint &pos);

    QString m_connID;
    bool m_connected = false;
    bool m_hideDefaultDBs = true;
    // Bumped on every reload; in-flight replies from an older generation are
    // dropped instead of appending a second copy of the tree.
    int m_gen = 0;

    QPushButton *m_adminBtn;
    QPushButton *m_schemaBtn;
    QStackedWidget *m_stack;
    QListWidget *m_admin;
    QLineEdit *m_filter;
    QPushButton *m_refreshBtn;
    QTreeWidget *m_tree;
    QSet<QString> m_loadedTables;  // schema
    QSet<QString> m_loadedColumns; // schema.table
};
