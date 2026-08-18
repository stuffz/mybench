import { useEffect } from "react";
import { Database, DatabaseZap } from "lucide-react";
import { startDrag } from "@/lib/drag";
import { useApp } from "@/store";
import { ConnDialog } from "@/components/ConnDialog";
import { AboutDialog } from "@/components/AboutDialog";
import { PrefsDialog } from "@/components/PrefsDialog";
import { ServerTabs } from "@/components/ServerTabs";
import { TabBar } from "@/components/TabBar";
import { Sidebar } from "@/components/Sidebar";
import { StatusStrip } from "@/components/StatusStrip";
import { EditorTab } from "@/views/EditorTab";
import { ProcesslistView } from "@/views/ProcesslistView";
import { UsersView } from "@/views/UsersView";
import { ServerInfoView } from "@/views/ServerInfoView";
import { InnoDBView } from "@/views/InnoDBView";
import { InspectorView } from "@/views/InspectorView";
import { GraphView } from "@/views/GraphView";
import { HistoryView } from "@/views/HistoryView";

// Shell: server tabs on top (one per open connection), the active server's
// query/panel tabs below; sidebar and status strip follow the active server.
// Tabs stay mounted so editor and result state survive switching. init()
// restores the persisted workspace; connections stay closed until the user
// connects each one explicitly (SPEC: session restore is lazy).

export default function App() {
  const { tabs, activeConn, activePerConn, restored, init } = useApp();
  const sideW = useApp((s) => s.sidebarWidth);
  const setSideW = useApp((s) => s.setSidebarWidth);

  useEffect(() => {
    void init();
    // init is a stable store action; run once on mount.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const visibleTab = activeConn ? activePerConn[activeConn] : undefined;

  return (
    <div className="flex h-screen flex-col bg-background text-foreground">
      <header className="flex h-11 shrink-0 items-center gap-3 border-b px-3">
        <span className="flex items-center gap-1.5 text-sm font-semibold tracking-tight">
          {/* Same lucide database glyph as the app icon, sans background. */}
          <Database className="h-4 w-4" />
          mybench
        </span>
        <ConnDialog />
        <div className="flex-1" />
        <PrefsDialog />
        <AboutDialog />
      </header>

      <ServerTabs />

      <div className="flex min-h-0 flex-1">
        {activeConn && (
          <>
            <Sidebar connID={activeConn} width={sideW} />
            <div
              className="relative z-10 -ml-0.5 -mr-0.5 w-1 shrink-0 cursor-col-resize hover:bg-primary/40"
              onMouseDown={(e) => {
                const start = sideW;
                startDrag(e, "col-resize", (dx) =>
                  setSideW(Math.min(560, Math.max(160, start + dx))),
                );
              }}
            />
          </>
        )}
        <div className="flex min-h-0 min-w-0 flex-1 flex-col">
          <TabBar />
          <div className="relative min-h-0 min-w-0 flex-1">
          {restored && tabs.length === 0 && (
            <div className="flex h-full flex-col items-center justify-center gap-2 text-muted-foreground">
              <DatabaseZap className="h-8 w-8 opacity-40" />
              <p className="text-sm">No open connections</p>
              <p className="text-xs">Use “Connections” in the header to connect to a server.</p>
            </div>
          )}
          {tabs.map((t) => {
            const isActive = t.tabID === visibleTab;
            return (
              <div key={t.tabID} className={"absolute inset-0 " + (isActive ? "" : "hidden")}>
                {t.view === "editor" && (
                  <EditorTab tabID={t.tabID} connID={t.connID} initialSQL={t.sql} />
                )}
                {t.view === "processlist" && (
                  <ProcesslistView connID={t.connID} active={isActive} />
                )}
                {t.view === "users" && <UsersView connID={t.connID} active={isActive} />}
                {t.view === "serverinfo" && (
                  <ServerInfoView connID={t.connID} active={isActive} />
                )}
                {t.view === "innodb" && <InnoDBView connID={t.connID} active={isActive} />}
                {t.view === "graph" && <GraphView connID={t.connID} active={isActive} />}
                {t.view === "history" && <HistoryView connID={t.connID} active={isActive} />}
                {(t.view === "tableinspect" || t.view === "schemainspect") && t.schema && (
                  <InspectorView
                    connID={t.connID}
                    schema={t.schema}
                    table={t.view === "tableinspect" ? t.table : undefined}
                    initialSection={t.section}
                    active={isActive}
                  />
                )}
              </div>
            );
          })}
          </div>
        </div>
      </div>

      {activeConn && <StatusStrip connID={activeConn} />}
    </div>
  );
}
