import { useCallback, useEffect, useState, type ReactNode } from "react";
import { AdminService } from "@/lib/api";
import type { SchemaColumn, IndexRow, ForeignKey, TableInfo, SchemaMeta } from "@/lib/api";
import { Button } from "@/components/ui/button";
import { useApp } from "@/store";

// Workbench-style inspector pages. Table: info | columns | indexes | fks |
// ddl. Schema: info | tables. Each section fetches lazily on first view.

type Props = {
  connID: string;
  schema: string;
  table?: string;
  initialSection?: string;
  active: boolean;
};

const TABLE_SECTIONS: [string, string][] = [
  ["info", "Info"],
  ["columns", "Columns"],
  ["indexes", "Indexes"],
  ["fks", "Foreign Keys"],
  ["ddl", "DDL"],
];

const SCHEMA_SECTIONS: [string, string][] = [
  ["info", "Info"],
  ["tables", "Tables"],
];

function fmtBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  const units = ["KiB", "MiB", "GiB", "TiB"];
  let v = n / 1024;
  let u = 0;
  while (v >= 1024 && u < units.length - 1) {
    v /= 1024;
    u++;
  }
  return `${v.toFixed(v < 10 ? 1 : 0)} ${units[u]}`;
}

const cell = "whitespace-nowrap px-2 py-1";
const th = "whitespace-nowrap px-2 py-1 font-normal";

function DataTable({ head, rows }: { head: string[]; rows: ReactNode[][] }) {
  return (
    <table className="w-full font-mono text-xs">
      <thead className="sticky top-0 bg-background text-left text-muted-foreground">
        <tr>
          {head.map((h) => (
            <th key={h} className={th}>
              {h}
            </th>
          ))}
        </tr>
      </thead>
      <tbody>
        {rows.map((r, i) => (
          <tr key={i} className="border-t border-border/40 hover:bg-muted/30">
            {r.map((c, j) => (
              <td key={j} className={cell}>
                {c}
              </td>
            ))}
          </tr>
        ))}
      </tbody>
    </table>
  );
}

function InfoList({ pairs }: { pairs: [string, ReactNode][] }) {
  return (
    <dl className="grid max-w-xl grid-cols-[12rem_1fr] gap-y-1 p-3 text-xs">
      {pairs.map(([k, v]) => (
        <div key={k} className="contents">
          <dt className="text-muted-foreground">{k}</dt>
          <dd className="font-mono">{v}</dd>
        </div>
      ))}
    </dl>
  );
}

export function InspectorView({ connID, schema, table, initialSection, active }: Props) {
  const addTab = useApp((s) => s.addTab);
  const sections = table ? TABLE_SECTIONS : SCHEMA_SECTIONS;
  const [section, setSection] = useState(
    sections.some(([k]) => k === initialSection) ? initialSection! : "info",
  );
  const [error, setError] = useState<string | null>(null);
  const [loaded, setLoaded] = useState<Set<string>>(new Set());

  const [info, setInfo] = useState<TableInfo | null>(null);
  const [columns, setColumns] = useState<SchemaColumn[]>([]);
  const [indexes, setIndexes] = useState<IndexRow[]>([]);
  const [fks, setFks] = useState<ForeignKey[]>([]);
  const [ddl, setDdl] = useState("");
  const [meta, setMeta] = useState<SchemaMeta | null>(null);
  const [schemaTables, setSchemaTables] = useState<TableInfo[]>([]);

  const load = useCallback(
    async (sec: string) => {
      setError(null);
      const clean = <T,>(a: (T | null)[] | null) => (a ?? []).filter((x): x is T => x !== null);
      try {
        if (table) {
          if (sec === "info") setInfo((await AdminService.TablesInfo(connID, schema, table))?.[0] ?? null);
          if (sec === "columns") setColumns(clean(await AdminService.Columns(connID, schema, table)));
          if (sec === "indexes") setIndexes(clean(await AdminService.Indexes(connID, schema, table)));
          if (sec === "fks") setFks(clean(await AdminService.ForeignKeys(connID, schema, table)));
          if (sec === "ddl") setDdl(await AdminService.ShowCreate(connID, schema, table));
        } else {
          if (sec === "info") {
            setMeta(await AdminService.SchemaInfo(connID, schema));
            setSchemaTables(clean(await AdminService.TablesInfo(connID, schema, "")));
          }
          if (sec === "tables") setSchemaTables(clean(await AdminService.TablesInfo(connID, schema, "")));
        }
        setLoaded((s) => new Set(s).add(sec));
      } catch (e) {
        setError(e instanceof Error ? e.message : String(e));
      }
    },
    [connID, schema, table],
  );

  const connected = useApp((s) => s.openIDs.includes(connID));
  useEffect(() => {
    if (active && connected && !loaded.has(section)) void load(section);
  }, [active, connected, section, loaded, load]);

  const refresh = () => {
    setLoaded(new Set());
    void load(section);
  };

  const bool = (b: boolean) => (b ? "yes" : "");

  return (
    <div className="flex h-full flex-col">
      <div className="flex h-[calc(2.5rem+1px)] shrink-0 items-center gap-1 border-b px-3 text-xs">
        <span className="mr-2 font-mono font-medium">
          {schema}
          {table ? `.${table}` : ""}
        </span>
        {sections.map(([key, label]) => (
          <button
            key={key}
            type="button"
            className={
              "rounded px-2 py-1 " +
              (section === key ? "bg-muted font-medium" : "text-muted-foreground hover:bg-muted/50")
            }
            onClick={() => setSection(key)}
          >
            {label}
          </button>
        ))}
        <div className="flex-1" />
        <Button size="xs" variant="outline" onClick={refresh}>
          Refresh
        </Button>
      </div>

      {error && (
        <pre className="m-2 whitespace-pre-wrap rounded-md border border-destructive/50 bg-destructive/10 p-2 font-mono text-xs text-destructive">
          {error}
        </pre>
      )}

      <div className="min-h-0 flex-1 overflow-auto">
        {table && section === "info" && info && (
          <InfoList
            pairs={[
              ["Type", info.type],
              ["Engine", info.engine],
              ["Row format", info.rowFormat],
              ["Rows (approx)", info.rows.toLocaleString()],
              ["Avg row length", fmtBytes(info.avgRowLength)],
              ["Data size", fmtBytes(info.dataLength)],
              ["Index size", fmtBytes(info.indexLength)],
              ["Auto increment", info.autoIncrement ? info.autoIncrement.toLocaleString() : "—"],
              ["Collation", info.collation],
              ["Created", info.created || "—"],
              ["Updated", info.updated || "—"],
              ["Comment", info.comment || "—"],
            ]}
          />
        )}

        {table && section === "columns" && (
          <DataTable
            head={["Column", "Type", "Key", "Nullable", "Default", "Extra", "Comment"]}
            rows={columns.map((c) => [
              c.name,
              c.type,
              c.key,
              bool(c.nullable),
              c.default,
              c.extra,
              <span key="c" className="text-muted-foreground">{c.comment}</span>,
            ])}
          />
        )}

        {table && section === "indexes" && (
          <DataTable
            head={["Index", "Columns", "Unique", "Type", "Cardinality"]}
            rows={indexes.map((i) => [i.name, i.columns, bool(i.unique), i.type, i.cardinality.toLocaleString()])}
          />
        )}

        {table && section === "fks" && (
          <>
            {fks.length === 0 && loaded.has("fks") && (
              <p className="p-3 text-xs text-muted-foreground">No foreign keys.</p>
            )}
            {fks.length > 0 && (
              <DataTable
                head={["Constraint", "Columns", "References", "Ref columns", "On update", "On delete"]}
                rows={fks.map((f) => [
                  f.name,
                  f.columns,
                  `${f.refSchema}.${f.refTable}`,
                  f.refColumns,
                  f.onUpdate,
                  f.onDelete,
                ])}
              />
            )}
          </>
        )}

        {table && section === "ddl" && (
          <pre className="whitespace-pre-wrap p-3 font-mono text-xs">{ddl}</pre>
        )}

        {!table && section === "info" && (
          <InfoList
            pairs={[
              ["Charset", meta?.charset ?? ""],
              ["Collation", meta?.collation ?? ""],
              ["Tables", schemaTables.filter((t) => t.type !== "VIEW").length],
              ["Views", schemaTables.filter((t) => t.type === "VIEW").length],
              ["Rows (approx)", schemaTables.reduce((a, t) => a + t.rows, 0).toLocaleString()],
              [
                "Total size",
                fmtBytes(schemaTables.reduce((a, t) => a + t.dataLength + t.indexLength, 0)),
              ],
            ]}
          />
        )}

        {!table && section === "tables" && (
          <DataTable
            head={["Table", "Type", "Engine", "Rows (approx)", "Data", "Indexes", "Collation", "Created"]}
            rows={schemaTables.map((t) => [
              <button
                key="n"
                type="button"
                className="text-left hover:underline"
                title="Open table inspector"
                onClick={() =>
                  addTab(connID, "tableinspect", { schema, table: t.name, section: "info" })
                }
              >
                {t.name}
              </button>,
              t.type === "VIEW" ? "view" : "table",
              t.engine,
              t.rows.toLocaleString(),
              fmtBytes(t.dataLength),
              fmtBytes(t.indexLength),
              t.collation,
              t.created,
            ])}
          />
        )}
      </div>
    </div>
  );
}
