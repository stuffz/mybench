#pragma once
// The workspace blob: serialises the tab tree, per-connection active tabs,
// prefs and sidebar width to the JSON the backend stores, and back. The
// format (version 2) predates this client — key names are the contract.
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QVector>

#include "shell/tabs.h"

// What a stored workspace decodes to. Titles are not part of the format —
// the caller derives them (editor tabs are numbered by arrival order).
struct WorkspaceState
{
    QVector<Tab> tabs;
    QHash<QString, QString> activePerConn; // connID → tabID
    QJsonObject prefs;
    int sidebarWidth = 240;
    int maxTabSeq = 0; // highest numeric tab id seen; new ids start above it
};

// Live editor widgets are consulted for their current SQL and height, so the
// snapshot reflects what is on screen, not the last synced copy. Pass
// sidebarWidth <= 0 when the sidebar is hidden; the previously stored width
// (in prefs) is then carried forward instead of being dropped.
QJsonObject encodeWorkspace(
    const QVector<Tab> &tabs, const QString &activeConn,
    const QHash<QString, QString> &activePerConn, const QJsonObject &prefs, int sidebarWidth
);

// Tabs whose connection is not in `knownConns` are dropped — their saved
// profile is gone, so they could never materialise.
WorkspaceState decodeWorkspace(const QJsonObject &ws, const QSet<QString> &knownConns);
