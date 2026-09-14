#pragma once
// Tab identity, shared by the sidebar, the tab bar and workspace persistence.
// TabView's string ids and the Tab fields are the workspace.json format
// (version 2) — renames here are format changes, not refactors.
#include <QString>

class QWidget;

enum class TabView
{
    Editor,
    Dashboard,
    Processlist,
    Users,
    ServerInfo,
    InnoDB,
    TableInspect,
    SchemaInspect,
    Graph,
    History,
};

// Panels that exist at most once per connection — reactivated, not duplicated.
inline bool isSingletonView(TabView v)
{
    switch (v)
    {
    case TabView::Dashboard:
    case TabView::Processlist:
    case TabView::Users:
    case TabView::ServerInfo:
    case TabView::InnoDB:
    case TabView::Graph:
    case TabView::History:
        return true;
    case TabView::Editor:
    case TabView::TableInspect:
    case TabView::SchemaInspect:
        return false;
    }
    // Unreachable while the enum is complete, which is the point: with no
    // default arm a new view has to be classified here instead of silently
    // becoming non-singleton and duplicating itself per connection.
    return false;
}

inline QString viewId(TabView v)
{
    switch (v)
    {
    case TabView::Editor:
        return "editor";
    case TabView::Dashboard:
        return "dashboard";
    case TabView::Processlist:
        return "processlist";
    case TabView::Users:
        return "users";
    case TabView::ServerInfo:
        return "serverinfo";
    case TabView::InnoDB:
        return "innodb";
    case TabView::TableInspect:
        return "tableinspect";
    case TabView::SchemaInspect:
        return "schemainspect";
    case TabView::Graph:
        return "graph";
    case TabView::History:
        return "history";
    }
    return "editor";
}

inline TabView viewFromId(const QString &s)
{
    if (s == "dashboard")
    {
        return TabView::Dashboard;
    }
    if (s == "processlist")
    {
        return TabView::Processlist;
    }
    if (s == "users")
    {
        return TabView::Users;
    }
    if (s == "serverinfo")
    {
        return TabView::ServerInfo;
    }
    if (s == "innodb")
    {
        return TabView::InnoDB;
    }
    if (s == "tableinspect")
    {
        return TabView::TableInspect;
    }
    if (s == "schemainspect")
    {
        return TabView::SchemaInspect;
    }
    if (s == "graph")
    {
        return TabView::Graph;
    }
    if (s == "history")
    {
        return TabView::History;
    }
    return TabView::Editor;
}

// Opening a tab: the view plus whatever target it needs.
struct TabRequest
{
    TabView view = TabView::Editor;
    QString schema;
    QString table;
    QString section;
    QString sql;
};

// A live (or restored-but-dormant) tab. `widget` is null until the tab's
// connection opens and the view is materialised; sql/editorH are the last
// synced copies used while the editor widget does not exist.
struct Tab
{
    QString tabID;
    QString connID;
    QString title;
    // User-renamed: restore keeps this title instead of re-deriving it.
    bool titled = false;
    TabView view = TabView::Editor;
    QString schema, table, section, sql;
    int editorH = 0;
    QWidget *widget = nullptr;
};

// Backquoted identifier for generated SQL.
inline QString quoteIdent(const QString &id)
{
    QString s = id;
    s.replace('`', QStringLiteral("``"));
    return "`" + s + "`";
}
