import { useCallback, useEffect, useState } from "react";
import { QueryService } from "@/lib/api";
import type { HistoryEntry } from "@/lib/api";
import { Button } from "@/components/ui/button";
import { useApp } from "@/store";

// Per-server query history (SQLite-backed): every executed statement with
// timing, row count and errors. Click an entry to open it in a new query tab.

type Props = { connID: string; active: boolean };

const LIMIT = 500;

function fmtWhen(rfc3339: string): string {
  const d = new Date(rfc3339);
  if (Number.isNaN(d.getTime())) return rfc3339;
  const today = new Date();
  const sameDay = d.toDateString() === today.toDateString();
  return sameDay ? d.toLocaleTimeString() : d.toLocaleString();
}

export function HistoryView({ connID, active }: Props) {
  const addTab = useApp((s) => s.addTab);
  const [items, setItems] = useState<HistoryEntry[]>([]);
  const [search, setSearch] = useState("");
  const [error, setError] = useState<string | null>(null);
  // Clear wipes the server's whole history — arm with a second click.
  const [armedClear, setArmedClear] = useState(false);

  const load = useCallback(() => {
    QueryService.History(connID, search, LIMIT)
      .then((r) => {
        setItems((r ?? []).filter((e): e is HistoryEntry => e !== null));
        setError(null);
      })
      .catch((e) => setError(e instanceof Error ? e.message : String(e)));
  }, [connID, search]);

  // Refresh whenever the tab becomes visible; live-filter with a small
  // debounce while typing.
  useEffect(() => {
    if (!active) return;
    const t = setTimeout(load, 150);
    return () => clearTimeout(t);
  }, [active, load]);

  const clear = () => {
    if (!armedClear) {
      setArmedClear(true);
      setTimeout(() => setArmedClear(false), 4000);
      return;
    }
    setArmedClear(false);
    QueryService.ClearHistory(connID)
      .then(load)
      .catch((e) => setError(e instanceof Error ? e.message : String(e)));
  };

  return (
    <div className="flex h-full flex-col">
      <div className="flex h-[calc(2.5rem+1px)] shrink-0 items-center gap-3 border-b px-3 text-xs">
        <span className="font-medium">Query History</span>
        <input
          placeholder="Search Statements…"
          className="h-6 w-56 rounded-md border bg-transparent px-2 text-xs outline-none focus:border-ring"
          value={search}
          onChange={(e) => setSearch(e.target.value)}
        />
        <Button size="xs" variant="outline" onClick={load}>
          Refresh
        </Button>
        <span className="text-muted-foreground">
          {items.length === LIMIT ? `latest ${LIMIT}` : `${items.length} statements`}
        </span>
        <div className="flex-1" />
        <Button size="xs" variant={armedClear ? "destructive" : "outline"} onClick={clear}>
          {armedClear ? "Really Clear All?" : "Clear History"}
        </Button>
      </div>
      {error && (
        <pre className="m-2 whitespace-pre-wrap rounded-md border border-destructive/50 bg-destructive/10 p-2 font-mono text-xs text-destructive">
          {error}
        </pre>
      )}
      <div className="min-h-0 flex-1 overflow-auto font-mono text-xs">
        {items.map((e) => (
          <button
            key={e.id}
            type="button"
            className="grid w-full grid-cols-[10rem_5rem_6rem_1fr] items-baseline gap-3 border-b border-border/40 px-3 py-1 text-left hover:bg-muted/40"
            title={e.query + "\n\nClick to open in a new query tab"}
            onClick={() => addTab(connID, "editor", { sql: e.query })}
          >
            <span className="truncate text-muted-foreground">{fmtWhen(e.startedAt)}</span>
            <span className="text-right tabular-nums text-muted-foreground">
              {e.durationMs.toLocaleString()} ms
            </span>
            {e.error ? (
              <span className="truncate text-destructive" title={e.error}>
                error
              </span>
            ) : (
              <span className="text-right tabular-nums text-muted-foreground">
                {e.rowCount.toLocaleString()} rows
              </span>
            )}
            <span className="min-w-0 truncate">
              {e.source === "mcp" && (
                <span className="mr-1.5 rounded bg-info/15 px-1 text-[10px] text-info">mcp</span>
              )}
              {e.query}
            </span>
          </button>
        ))}
        {items.length === 0 && !error && (
          <p className="p-3 text-muted-foreground">No statements recorded yet.</p>
        )}
      </div>
    </div>
  );
}
