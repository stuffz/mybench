#include "shell/workspace.h"

#include "editor/editortab.h"
#include "shell/tabs.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QStringView>

QJsonObject encodeWorkspace(
    const QVector<Tab> &tabs, const QString &activeConn,
    const QHash<QString, QString> &activePerConn, const QJsonObject &prefs, int sidebarWidth
)
{
    QJsonArray tabsJson;
    for (const Tab &t : tabs)
    {
        QJsonObject o;
        o.insert("tabID", t.tabID);
        o.insert("connID", t.connID);
        o.insert("title", t.title);
        if (t.titled)
        {
            o.insert("titled", true);
        }
        o.insert("view", viewId(t.view));
        if (!t.schema.isEmpty())
        {
            o.insert("schema", t.schema);
        }
        if (!t.table.isEmpty())
        {
            o.insert("table", t.table);
        }
        if (!t.section.isEmpty())
        {
            o.insert("section", t.section);
        }
        if (t.view == TabView::Editor)
        {
            auto *e = qobject_cast<EditorTab *>(t.widget);
            o.insert("sql", e ? e->sql() : t.sql);
            const int h = e ? e->editorHeight() : t.editorH;
            if (h > 0)
            {
                o.insert("editorH", h);
            }
        }
        tabsJson.append(o);
    }

    QJsonObject activeJson;
    for (auto it = activePerConn.constBegin(); it != activePerConn.constEnd(); ++it)
    {
        activeJson.insert(it.key(), it.value());
    }

    QJsonObject ws;
    ws.insert("version", 2);
    ws.insert("tabs", tabsJson);
    ws.insert("activeConn", activeConn.isEmpty() ? QJsonValue() : QJsonValue(activeConn));
    ws.insert("activePerConn", activeJson);
    ws.insert("prefs", prefs);
    if (sidebarWidth > 0)
    {
        ws.insert("sidebarWidth", sidebarWidth);
    }
    else if (prefs.contains("sidebarWidth"))
    {
        ws.insert("sidebarWidth", prefs.value("sidebarWidth"));
    }
    return ws;
}

WorkspaceState decodeWorkspace(const QJsonObject &ws, const QSet<QString> &knownConns)
{
    WorkspaceState st;
    st.prefs = ws.value("prefs").toObject();
    st.sidebarWidth = ws.value("sidebarWidth").toInt(240);

    for (const auto &v : ws.value("tabs").toArray())
    {
        const QJsonObject o = v.toObject();
        const QString connID = o.value("connID").toString();
        if (!knownConns.contains(connID))
        {
            continue;
        }
        Tab t;
        t.tabID = o.value("tabID").toString();
        t.connID = connID;
        t.title = o.value("title").toString();
        t.titled = o.value("titled").toBool();
        t.view = viewFromId(o.value("view").toString());
        t.schema = o.value("schema").toString();
        t.table = o.value("table").toString();
        t.section = o.value("section").toString();
        t.sql = o.value("sql").toString();
        t.editorH = o.value("editorH").toInt();
        const int n = QStringView(t.tabID).mid(1).toInt();
        if (n > st.maxTabSeq)
        {
            st.maxTabSeq = n;
        }
        st.tabs.append(t);
    }

    const QJsonObject apc = ws.value("activePerConn").toObject();
    for (auto it = apc.constBegin(); it != apc.constEnd(); ++it)
    {
        st.activePerConn.insert(it.key(), it.value().toString());
    }
    return st;
}
