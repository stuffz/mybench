import { useCallback, useEffect, useMemo, useState } from "react";
import { AdminService } from "@/lib/api";
import type { ServerInfo, KV } from "@/lib/api";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { useApp } from "@/store";

// Server-status page (Workbench's "Server Status"): summary cards from the
// interesting variables/counters, then the full SHOW GLOBAL VARIABLES /
// STATUS lists behind one filter box.

type Props = { connID: string; active: boolean };

function fmtUptime(s: number): string {
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  return d > 0 ? `${d}d ${h}h ${m}m` : h > 0 ? `${h}h ${m}m` : `${m}m ${s % 60}s`;
}

function fmtBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  const units = ["KiB", "MiB", "GiB", "TiB"];
  let v = n / 1024;
  let u = 0;
  while (v >= 1024 && u < units.length - 1) {
    v /= 1024;
    u++;
  }
  return `${v.toFixed(1)} ${units[u]}`;
}

function Card({ label, value }: { label: string; value: string }) {
  return (
    <div className="rounded-md border p-2">
      <div className="text-[10px] uppercase tracking-wide text-muted-foreground">{label}</div>
      <div className="truncate font-mono text-sm" title={value}>
        {value}
      </div>
    </div>
  );
}

function KVTable({ title, rows, filter }: { title: string; rows: KV[]; filter: string }) {
  const f = filter.toLowerCase();
  const shown = f ? rows.filter((r) => r.name.toLowerCase().includes(f)) : rows;
  return (
    <div className="flex min-h-0 min-w-0 flex-1 flex-col">
      <div className="border-b px-2 py-1 text-xs font-medium">
        {title} <span className="text-muted-foreground">({shown.length})</span>
      </div>
      <div className="min-h-0 flex-1 overflow-auto">
        <table className="w-full font-mono text-xs">
          <tbody>
            {shown.map((r) => (
              <tr key={r.name} className="border-t border-border/40 hover:bg-muted/30">
                <td className="whitespace-nowrap px-2 py-0.5">{r.name}</td>
                <td className="max-w-96 truncate px-2 py-0.5 text-muted-foreground" title={r.value}>
                  {r.value}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}

export function ServerInfoView({ connID, active }: Props) {
  const [info, setInfo] = useState<ServerInfo | null>(null);
  const [filter, setFilter] = useState("");
  const [error, setError] = useState<string | null>(null);

  const refresh = useCallback(async () => {
    try {
      setInfo(await AdminService.ServerInfoSnapshot(connID));
      setError(null);
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }, [connID]);

  const connected = useApp((s) => s.openIDs.includes(connID));
  useEffect(() => {
    if (active && connected && !info) void refresh();
  }, [active, connected, info, refresh]);

  const vars = useMemo(
    () => (info?.variables ?? []).filter((v): v is KV => v !== null),
    [info],
  );
  const status = useMemo(
    () => (info?.status ?? []).filter((v): v is KV => v !== null),
    [info],
  );
  const v = useMemo(() => new Map(vars.map((x) => [x.name, x.value])), [vars]);
  const st = useMemo(() => new Map(status.map((x) => [x.name, x.value])), [status]);
  const num = (m: Map<string, string>, k: string) => parseInt(m.get(k) ?? "0", 10) || 0;

  return (
    <div className="flex h-full flex-col">
      <div className="flex h-[calc(2.5rem+1px)] shrink-0 items-center gap-3 border-b px-3 text-xs">
        <span className="font-medium">Server Info</span>
        <Button size="xs" variant="outline" onClick={() => void refresh()}>
          Refresh
        </Button>
        <Input
          className="h-7 w-64 text-xs"
          placeholder="Filter variables / status…"
          value={filter}
          onChange={(e) => setFilter(e.target.value)}
        />
      </div>

      {error && (
        <pre className="m-2 whitespace-pre-wrap rounded-md border border-destructive/50 bg-destructive/10 p-2 font-mono text-xs text-destructive">
          {error}
        </pre>
      )}

      {info && (
        <>
          <div className="grid grid-cols-3 gap-2 border-b p-2 lg:grid-cols-6">
            <Card label="Version" value={`${v.get("version") ?? "?"} ${v.get("version_comment") ?? ""}`} />
            <Card label="Host" value={`${v.get("hostname") ?? "?"}:${v.get("port") ?? "?"}`} />
            <Card label="OS" value={v.get("version_compile_os") ?? "?"} />
            <Card label="Uptime" value={fmtUptime(num(st, "Uptime"))} />
            <Card
              label="Threads"
              value={`${num(st, "Threads_connected")} conn · ${num(st, "Threads_running")} running`}
            />
            <Card label="Questions" value={num(st, "Questions").toLocaleString()} />
            <Card label="Data dir" value={v.get("datadir") ?? "?"} />
            <Card label="InnoDB buffer pool" value={fmtBytes(num(v, "innodb_buffer_pool_size"))} />
            <Card label="Max connections" value={String(num(v, "max_connections"))} />
            <Card label="Total connections" value={num(st, "Connections").toLocaleString()} />
            <Card label="Received" value={fmtBytes(num(st, "Bytes_received"))} />
            <Card label="Sent" value={fmtBytes(num(st, "Bytes_sent"))} />
          </div>

          <div className="flex min-h-0 flex-1 divide-x">
            <KVTable title="Global Variables" rows={vars} filter={filter} />
            <KVTable title="Global Status" rows={status} filter={filter} />
          </div>
        </>
      )}
    </div>
  );
}
