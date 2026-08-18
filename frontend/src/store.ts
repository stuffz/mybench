import { create } from "zustand";
import { ConnectionService, WorkspaceService } from "@/lib/api";
import type { SavedConn } from "@/lib/api";
import { DEFAULT_PREFS, applyPrefs, type Prefs } from "@/lib/prefs";

// App state only (SPEC.md frontend architecture): saved/open connections and
// the tab tree. Two levels: server tabs (one per open connection) each own a
// row of query/panel tabs. Result data never comes near this store. The tab
// tree (and editor SQL) persists to workspace.json through the backend so the
// app reopens where it left off.

export type TabView =
  | "editor"
  | "processlist"
  | "users"
  | "serverinfo"
  | "innodb"
  | "tableinspect"
  | "schemainspect"
  | "graph"
  | "history";

// Panels that exist at most once per connection — reactivated, not duplicated.
const SINGLETON_VIEWS: TabView[] = [
  "processlist",
  "users",
  "serverinfo",
  "innodb",
  "graph",
  "history",
];

export type Tab = {
  tabID: string;
  connID: string;
  title: string;
  view: TabView;
  // tableinspect / schemainspect targets
  schema?: string;
  table?: string;
  section?: string;
  // editor tabs: last known document, kept current for persistence
  sql?: string;
  // editor tabs: editor pane height from the splitter
  editorH?: number;
};

let tabSeq = 0;

type AddTabOpts = { schema?: string; table?: string; section?: string; sql?: string };

type AppState = {
  saved: SavedConn[];
  openIDs: string[];
  tabs: Tab[];
  activeConn: string | null;
  // Per connection: which of its tabs is showing.
  activePerConn: Record<string, string>;
  prefs: Prefs;
  sidebarWidth: number;
  restored: boolean;
  setPrefs: (patch: Partial<Prefs>) => void;
  setSidebarWidth: (w: number) => void;
  setTabEditorH: (tabID: string, h: number) => void;
  init: () => Promise<void>;
  refreshSaved: () => Promise<void>;
  openConn: (id: string) => Promise<void>;
  closeConn: (id: string) => void;
  setActiveConn: (id: string) => void;
  addTab: (connID: string, view: TabView, opts?: AddTabOpts) => void;
  // "Show in Graph": focus request per connection. Held in the store (not
  // tab opts) because the graph tab is a singleton that may already exist;
  // seq retriggers focusing the same table twice.
  graphFocus: Record<string, { schema: string; table: string; seq: number }>;
  showInGraph: (connID: string, schema: string, table: string) => void;
  closeTab: (tabID: string) => void;
  setActive: (tabID: string) => void;
  setTabSQL: (tabID: string, sql: string) => void;
};

function tabTitle(tabs: Tab[], connID: string, view: TabView, opts?: AddTabOpts): string {
  switch (view) {
    case "editor": {
      // Number query tabs per server; reuse of closed numbers is fine.
      const n = tabs.filter((t) => t.connID === connID && t.view === "editor").length + 1;
      return `Query ${n}`;
    }
    case "processlist":
      return "Client Connections";
    case "users":
      return "Users";
    case "serverinfo":
      return "Server Info";
    case "innodb":
      return "InnoDB Status";
    case "graph":
      return "Schema Graph";
    case "history":
      return "Query History";
    case "tableinspect":
      return `${opts?.schema}.${opts?.table}`;
    case "schemainspect":
      return `${opts?.schema} (schema)`;
  }
}

export const useApp = create<AppState>((set, get) => ({
  saved: [],
  openIDs: [],
  tabs: [],
  activeConn: null,
  activePerConn: {},
  prefs: DEFAULT_PREFS,
  sidebarWidth: 240,
  restored: false,

  setPrefs: (patch) => {
    const prefs = { ...get().prefs, ...patch };
    applyPrefs(prefs);
    set({ prefs });
  },

  setSidebarWidth: (w) => set({ sidebarWidth: w }),

  setTabEditorH: (tabID, h) =>
    set((s) => ({
      tabs: s.tabs.map((t) => (t.tabID === tabID ? { ...t, editorH: h } : t)),
    })),

  // Startup: load saved profiles and restore the persisted workspace, but
  // never dial anything — the app always boots with zero open connections.
  // Restored connections render as "connect" stubs in the server tab row
  // and open only when explicitly clicked.
  init: async () => {
    await get().refreshSaved();
    try {
      const raw = await WorkspaceService.Load();
      if (raw) {
        const ws = JSON.parse(raw) as {
          tabs?: Tab[];
          activeConn?: string | null;
          activePerConn?: Record<string, string>;
          prefs?: Partial<Prefs>;
          sidebarWidth?: number;
        };
        if (ws.prefs) {
          const prefs = { ...DEFAULT_PREFS, ...ws.prefs };
          applyPrefs(prefs);
          set({ prefs });
        }
        if (ws.sidebarWidth) set({ sidebarWidth: ws.sidebarWidth });
        const known = new Set(get().saved.map((c) => c.id));
        const kept = (ws.tabs ?? []).filter((t) => known.has(t.connID));
        // Recompute titles so older workspace files match the current scheme.
        const tabs: Tab[] = [];
        for (const t of kept) {
          tabs.push({ ...t, title: tabTitle(tabs, t.connID, t.view, t) });
        }
        for (const t of tabs) {
          const n = parseInt(t.tabID.replace(/^t/, ""), 10);
          if (Number.isFinite(n) && n > tabSeq) tabSeq = n;
        }
        const connIDs = [...new Set(tabs.map((t) => t.connID))];
        const activePerConn: Record<string, string> = {};
        for (const id of connIDs) {
          const wanted = ws.activePerConn?.[id];
          const own = tabs.filter((t) => t.connID === id);
          activePerConn[id] = own.some((t) => t.tabID === wanted)
            ? wanted!
            : own[0].tabID;
        }
        set({
          tabs,
          activePerConn,
          activeConn: null,
        });
      }
    } catch {
      // A corrupt workspace file must never block startup; start empty.
    }
    set({ restored: true });
  },

  refreshSaved: async () => {
    const list = await ConnectionService.List();
    set({ saved: (list ?? []).filter((c): c is SavedConn => c !== null) });
  },

  openConn: async (id) => {
    await ConnectionService.Open(id);
    set((s) => ({
      openIDs: s.openIDs.includes(id) ? s.openIDs : [...s.openIDs, id],
      activeConn: id,
    }));
    if (!get().tabs.some((t) => t.connID === id)) get().addTab(id, "editor");
  },

  closeConn: (id) => {
    void ConnectionService.Close(id);
    set((s) => {
      const tabs = s.tabs.filter((t) => t.connID !== id);
      const { [id]: _gone, ...activePerConn } = s.activePerConn;
      const openIDs = s.openIDs.filter((o) => o !== id);
      return {
        openIDs,
        tabs,
        activePerConn,
        activeConn: s.activeConn === id ? (openIDs[0] ?? null) : s.activeConn,
      };
    });
  },

  setActiveConn: (id) => set({ activeConn: id }),

  addTab: (connID, view, opts) => {
    if (SINGLETON_VIEWS.includes(view)) {
      const existing = get().tabs.find((t) => t.connID === connID && t.view === view);
      if (existing) {
        get().setActive(existing.tabID);
        return;
      }
    }
    tabSeq += 1;
    const tab: Tab = {
      tabID: `t${tabSeq}`,
      connID,
      title: tabTitle(get().tabs, connID, view, opts),
      view,
      ...opts,
    };
    set((s) => ({
      tabs: [...s.tabs, tab],
      activeConn: connID,
      activePerConn: { ...s.activePerConn, [connID]: tab.tabID },
    }));
  },

  graphFocus: {},
  showInGraph: (connID, schema, table) => {
    set((s) => ({
      graphFocus: {
        ...s.graphFocus,
        [connID]: { schema, table, seq: (s.graphFocus[connID]?.seq ?? 0) + 1 },
      },
    }));
    get().addTab(connID, "graph");
  },

  closeTab: (tabID) => {
    set((s) => {
      const closed = s.tabs.find((t) => t.tabID === tabID);
      const tabs = s.tabs.filter((t) => t.tabID !== tabID);
      const activePerConn = { ...s.activePerConn };
      if (closed && activePerConn[closed.connID] === tabID) {
        const own = tabs.filter((t) => t.connID === closed.connID);
        if (own.length > 0) {
          activePerConn[closed.connID] = own[own.length - 1].tabID;
        } else {
          delete activePerConn[closed.connID];
        }
      }
      return { tabs, activePerConn };
    });
  },

  setActive: (tabID) => {
    const tab = get().tabs.find((t) => t.tabID === tabID);
    if (!tab) return;
    set((s) => ({
      activeConn: tab.connID,
      activePerConn: { ...s.activePerConn, [tab.connID]: tabID },
    }));
  },

  setTabSQL: (tabID, sql) =>
    set((s) => ({
      tabs: s.tabs.map((t) => (t.tabID === tabID ? { ...t, sql } : t)),
    })),
}));

// Persist the tab tree write-through to SQLite; only after restore so an
// early render can't wipe the saved workspace with an empty one. The tiny
// coalesce window keeps splitter drags from firing a save per mousemove
// while still landing the final state effectively instantly.
const SAVE_COALESCE_MS = 50;
let saveTimer: ReturnType<typeof setTimeout> | null = null;
let lastSaved = "";
let pendingBlob = "";
function flushSave() {
  if (saveTimer) {
    clearTimeout(saveTimer);
    saveTimer = null;
  }
  if (pendingBlob && pendingBlob !== lastSaved) {
    lastSaved = pendingBlob;
    void WorkspaceService.Save(pendingBlob).catch(() => {});
  }
}
useApp.subscribe((s) => {
  if (!s.restored) return;
  const blob = JSON.stringify({
    version: 2,
    tabs: s.tabs,
    activeConn: s.activeConn,
    activePerConn: s.activePerConn,
    prefs: s.prefs,
    sidebarWidth: s.sidebarWidth,
  });
  if (blob === lastSaved) return;
  pendingBlob = blob;
  if (saveTimer) clearTimeout(saveTimer);
  saveTimer = setTimeout(flushSave, SAVE_COALESCE_MS);
});
// Best-effort final flush when the window goes away mid-coalesce.
window.addEventListener("pagehide", flushSave);

// Stable per-connection accent so prod and dev are never confused.
export function connHue(connID: string): number {
  let h = 0;
  for (const ch of connID) h = (h * 31 + ch.charCodeAt(0)) % 360;
  return h;
}

// The connection's accent: the user-picked color, or the stable auto hue.
export function connColor(saved: SavedConn[], id: string): string {
  const c = saved.find((x) => x.id === id);
  return c?.color || `hsl(${connHue(id)} 60% 50%)`;
}
