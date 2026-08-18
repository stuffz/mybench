import { Plus, ChevronDown, X } from "lucide-react";
import { useApp, connColor } from "@/store";
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu";

// Second level of the tab tree: the active server's query and panel tabs,
// plus "+" for a new query tab and the panel menu (server info, client
// connections, …) — Workbench-style.

export function TabBar() {
  const { saved, tabs, activeConn, activePerConn, addTab, closeTab, setActive } = useApp();
  if (!activeConn) return null;

  const own = tabs.filter((t) => t.connID === activeConn);
  const activeTab = activePerConn[activeConn];
  const accent = connColor(saved, activeConn);

  return (
    <div className="flex items-center gap-1 border-b px-2 py-1">
      {own.map((t) => (
        <div
          key={t.tabID}
          className={
            "flex h-7 cursor-pointer items-center gap-1.5 rounded-md border px-2.5 text-xs " +
            (t.tabID === activeTab
              ? "bg-background shadow-sm"
              : "border-transparent text-muted-foreground hover:bg-muted/50 hover:text-foreground")
          }
          onClick={() => setActive(t.tabID)}
        >
          <span
            className="h-1.5 w-1.5 shrink-0 rounded-full"
            style={{ background: accent, opacity: t.view === "editor" ? 1 : 0.6 }}
          />
          {t.title}
          <button
            type="button"
            className="-mr-0.5 rounded p-0.5 text-muted-foreground hover:bg-muted hover:text-foreground"
            title="Close tab"
            onClick={(e) => {
              e.stopPropagation();
              closeTab(t.tabID);
            }}
          >
            <X className="h-3 w-3" />
          </button>
        </div>
      ))}
      <button
        type="button"
        className="grid h-7 w-7 place-items-center rounded-md text-muted-foreground hover:bg-muted/50 hover:text-foreground"
        title="New query tab"
        onClick={() => addTab(activeConn, "editor")}
      >
        <Plus className="h-3.5 w-3.5" />
      </button>
      <DropdownMenu>
        <DropdownMenuTrigger
          className="grid h-7 w-7 place-items-center rounded-md text-muted-foreground hover:bg-muted/50 hover:text-foreground"
          title="Server panels"
        >
          <ChevronDown className="h-3.5 w-3.5" />
        </DropdownMenuTrigger>
        <DropdownMenuContent align="start" className="text-xs">
          <DropdownMenuItem onClick={() => addTab(activeConn, "serverinfo")}>
            Server Info
          </DropdownMenuItem>
          <DropdownMenuItem onClick={() => addTab(activeConn, "innodb")}>
            InnoDB Status
          </DropdownMenuItem>
          <DropdownMenuItem onClick={() => addTab(activeConn, "processlist")}>
            Client Connections
          </DropdownMenuItem>
          <DropdownMenuItem onClick={() => addTab(activeConn, "users")}>
            Users and Privileges
          </DropdownMenuItem>
          <DropdownMenuItem onClick={() => addTab(activeConn, "history")}>
            Query History
          </DropdownMenuItem>
          <DropdownMenuItem onClick={() => addTab(activeConn, "graph")}>
            Schema Graph
          </DropdownMenuItem>
        </DropdownMenuContent>
      </DropdownMenu>
    </div>
  );
}
