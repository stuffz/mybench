import { useEffect, useState, type MouseEvent } from "react";
import { Gauge, Database, Network, Users, Waypoints, Table2, Eye, History } from "lucide-react";
import { AdminService } from "@/lib/api";
import type { SchemaTable, SchemaColumn } from "@/lib/api";
import { Input } from "@/components/ui/input";
import { useApp } from "@/store";
import { ContextMenu, type CtxMenuItem } from "@/components/ContextMenu";

// Sidebar for the active server, Workbench-style: an Administration panel
// (server pages) and a Schemas panel (filter box + schema → table → column
// tree, lazy-loaded from information_schema). Left-click only selects/
// expands — everything that acts (new query tab, inspectors) lives on
// right-click.

type Props = { connID: string; width: number };

type Menu = { x: number; y: number; schema: string; table?: string };

// MySQL's own schemas, hidden by the "Hide Default Databases" preference.
const DEFAULT_DBS = new Set(["information_schema", "performance_schema", "mysql", "sys"]);

const ADMIN_PAGES = [
  ["serverinfo", "Server Status", Gauge],
  ["innodb", "InnoDB Status", Database],
  ["processlist", "Client Connections", Network],
  ["users", "Users and Privileges", Users],
  ["graph", "Schema Graph", Waypoints],
  ["history", "Query History", History],
] as const;

export function Sidebar({ connID, width }: Props) {
  const addTab = useApp((s) => s.addTab);
  const showInGraph = useApp((s) => s.showInGraph);
  const hideDefaultDBs = useApp((s) => s.prefs.hideDefaultDBs);
  // Restore opens connections asynchronously — don't fetch (and error) early.
  const connected = useApp((s) => s.openIDs.includes(connID));
  const [mode, setMode] = useState<"admin" | "schemas">("schemas");
  const [filter, setFilter] = useState("");
  const [schemas, setSchemas] = useState<string[]>([]);
  const [openSchemas, setOpenSchemas] = useState<Set<string>>(new Set());
  const [tables, setTables] = useState<Map<string, SchemaTable[]>>(new Map());
  const [openTables, setOpenTables] = useState<Set<string>>(new Set());
  const [columns, setColumns] = useState<Map<string, SchemaColumn[]>>(new Map());
  const [selected, setSelected] = useState<string | null>(null);
  const [menu, setMenu] = useState<Menu | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    setSchemas([]);
    setOpenSchemas(new Set());
    setTables(new Map());
    setOpenTables(new Set());
    setColumns(new Map());
    setSelected(null);
    setError(null);
    if (!connected) return;
    AdminService.Schemas(connID)
      .then((s) => setSchemas((s ?? []).filter((x): x is string => x !== null)))
      .catch((e) => setError(e instanceof Error ? e.message : String(e)));
  }, [connID, connected]);

  const toggleSchema = async (schema: string) => {
    const next = new Set(openSchemas);
    if (next.has(schema)) {
      next.delete(schema);
    } else {
      next.add(schema);
      if (!tables.has(schema)) {
        try {
          const t = await AdminService.Tables(connID, schema);
          setTables((m) =>
            new Map(m).set(schema, (t ?? []).filter((x): x is SchemaTable => x !== null)),
          );
        } catch (e) {
          setError(e instanceof Error ? e.message : String(e));
        }
      }
    }
    setOpenSchemas(next);
  };

  const toggleTable = async (schema: string, table: string) => {
    const key = `${schema}.${table}`;
    const next = new Set(openTables);
    if (next.has(key)) {
      next.delete(key);
    } else {
      next.add(key);
      if (!columns.has(key)) {
        try {
          const c = await AdminService.Columns(connID, schema, table);
          setColumns((m) =>
            new Map(m).set(key, (c ?? []).filter((x): x is SchemaColumn => x !== null)),
          );
        } catch (e) {
          setError(e instanceof Error ? e.message : String(e));
        }
      }
    }
    setOpenTables(next);
  };

  const onContext = (e: MouseEvent, schema: string, table?: string) => {
    e.preventDefault();
    setSelected(table ? `${schema}.${table}` : schema);
    setMenu({ x: e.clientX, y: e.clientY, schema, table });
  };

  const q = (id: string) => "`" + id.replace(/`/g, "``") + "`";
  // Context menu "Open in New Query Tab" and double-clicking a table row.
  const openQueryTab = (schema: string, table: string) =>
    addTab(connID, "editor", {
      sql: `SELECT * FROM ${q(schema)}.${q(table)} LIMIT 200;`,
    });

  const menuItems = (m: Menu): CtxMenuItem[] => {
    if (m.table) {
      const target = { schema: m.schema, table: m.table };
      return [
        {
          label: "Open in New Query Tab",
          onClick: () => openQueryTab(m.schema, m.table!),
        },
        { separator: true },
        { label: "Table Inspector", onClick: () => addTab(connID, "tableinspect", { ...target, section: "info" }) },
        { label: "Columns", onClick: () => addTab(connID, "tableinspect", { ...target, section: "columns" }) },
        { label: "Indexes", onClick: () => addTab(connID, "tableinspect", { ...target, section: "indexes" }) },
        { label: "Foreign Keys", onClick: () => addTab(connID, "tableinspect", { ...target, section: "fks" }) },
        { label: "DDL", onClick: () => addTab(connID, "tableinspect", { ...target, section: "ddl" }) },
        { separator: true },
        { label: "Show in Graph", onClick: () => showInGraph(connID, m.schema, m.table!) },
        { separator: true },
        {
          label: "Copy Name",
          onClick: () => void navigator.clipboard.writeText(`${m.schema}.${m.table}`),
        },
      ];
    }
    return [
      {
        label: "New Query Tab on This Schema",
        onClick: () => addTab(connID, "editor", { sql: `USE ${q(m.schema)};\n` }),
      },
      {
        label: "Schema Inspector",
        onClick: () => addTab(connID, "schemainspect", { schema: m.schema, section: "info" }),
      },
      { separator: true },
      { label: "Copy Name", onClick: () => void navigator.clipboard.writeText(m.schema) },
    ];
  };

  const match = (s: string) => s.toLowerCase().includes(filter.toLowerCase());
  const userSchemas = hideDefaultDBs ? schemas.filter((s) => !DEFAULT_DBS.has(s)) : schemas;
  const shownSchemas = filter
    ? userSchemas.filter(
        (s) => match(s) || (tables.get(s) ?? []).some((t) => match(t.name)),
      )
    : userSchemas;

  return (
    <div
      className="flex shrink-0 flex-col overflow-auto border-r font-mono text-xs"
      style={{ width }}
    >
      <div className="flex gap-1 border-b p-1.5 font-sans">
        {(
          [
            ["admin", "Administration"],
            ["schemas", "Schemas"],
          ] as const
        ).map(([key, label]) => (
          <button
            key={key}
            type="button"
            className={
              "flex-1 rounded-md px-2 py-1 " +
              (mode === key
                ? "bg-muted font-medium"
                : "text-muted-foreground hover:bg-muted/50 hover:text-foreground")
            }
            onClick={() => setMode(key)}
          >
            {label}
          </button>
        ))}
      </div>
      {error && <div className="p-2 text-destructive">{error}</div>}
      {!connected && !error && (
        <div className="p-2 font-sans text-muted-foreground">Connecting…</div>
      )}

      {mode === "admin" && (
        <div className="flex flex-col gap-0.5 p-1.5 font-sans">
          {ADMIN_PAGES.map(([view, label, Icon]) => (
            <button
              key={view}
              type="button"
              className="flex items-center gap-2 rounded-md px-2 py-1.5 text-left hover:bg-muted/50"
              onClick={() => addTab(connID, view)}
            >
              <Icon className="h-3.5 w-3.5 shrink-0 text-muted-foreground" />
              {label}
            </button>
          ))}
        </div>
      )}

      {mode === "schemas" && (
        <div className="border-b p-1.5">
          <Input
            className="h-7 text-xs"
            placeholder="Filter objects"
            value={filter}
            onChange={(e) => setFilter(e.target.value)}
          />
        </div>
      )}

      {mode === "schemas" &&
        shownSchemas.map((schema) => (
        <div key={schema}>
          <button
            type="button"
            className={
              "flex w-full items-center gap-1.5 px-2 py-1 text-left hover:bg-muted/50 " +
              (selected === schema ? "bg-muted/60" : "")
            }
            onClick={() => void toggleSchema(schema)}
            onContextMenu={(e) => onContext(e, schema)}
          >
            <span className="text-muted-foreground">{openSchemas.has(schema) ? "▾" : "▸"}</span>
            <Database className="h-3.5 w-3.5 shrink-0 text-warning/80" />
            <span className="min-w-0 truncate">{schema}</span>
          </button>
          {openSchemas.has(schema) &&
            (tables.get(schema) ?? [])
              .filter((t) => !filter || match(t.name) || match(schema))
              .map((t) => {
              const key = `${schema}.${t.name}`;
              return (
                <div key={key}>
                  <div
                    className={
                      "flex w-full items-center pl-5 pr-2 hover:bg-muted/50 " +
                      (selected === key ? "bg-muted/60" : "")
                    }
                    onContextMenu={(e) => onContext(e, schema, t.name)}
                  >
                    <button
                      type="button"
                      className="py-0.5 pr-1 text-muted-foreground"
                      title="Show columns"
                      onClick={() => void toggleTable(schema, t.name)}
                    >
                      {openTables.has(key) ? "▾" : "▸"}
                    </button>
                    <button
                      type="button"
                      className="flex min-w-0 flex-1 items-center gap-1.5 py-0.5 text-left"
                      title={t.type === "VIEW" ? `${key} (view)` : key}
                      onClick={() => setSelected(key)}
                      onDoubleClick={() => openQueryTab(schema, t.name)}
                    >
                      {t.type === "VIEW" ? (
                        <Eye className="h-3.5 w-3.5 shrink-0 text-special/80" />
                      ) : (
                        <Table2 className="h-3.5 w-3.5 shrink-0 text-info/80" />
                      )}
                      <span className="min-w-0 truncate">{t.name}</span>
                    </button>
                  </div>
                  {openTables.has(key) &&
                    (columns.get(key) ?? []).map((c) => (
                      <div key={c.name} className="truncate py-0.5 pl-12 pr-2 text-muted-foreground">
                        {c.name}
                        <span className="ml-1 text-[10px]">
                          {c.type}
                          {c.key === "PRI" ? " 🔑" : ""}
                        </span>
                      </div>
                    ))}
                </div>
              );
            })}
        </div>
      ))}
      {menu && (
        <ContextMenu x={menu.x} y={menu.y} items={menuItems(menu)} onClose={() => setMenu(null)} />
      )}
    </div>
  );
}
