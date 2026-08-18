import { useEffect, useRef, useState, type ReactNode } from "react";
import { Database, Clock, Cpu, Activity } from "lucide-react";
import { AdminService } from "@/lib/api";
import type { StatusSnapshot } from "@/lib/api";
import { useApp } from "@/store";

// Footer status for the active server: a health dot (green = last poll
// succeeded, red = failed, with details on hover), then version, uptime,
// threads and QPS from the Questions delta. Polls only while mounted.

const POLL_MS = 5000;

type Props = { connID: string };

export function StatusStrip({ connID }: Props) {
  const saved = useApp((s) => s.saved);
  const [snap, setSnap] = useState<StatusSnapshot | null>(null);
  const [qps, setQps] = useState<number | null>(null);
  const [latency, setLatency] = useState<number | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [checkedAt, setCheckedAt] = useState<Date | null>(null);
  const prev = useRef<{ q: number; at: number } | null>(null);

  useEffect(() => {
    prev.current = null;
    setSnap(null);
    setQps(null);
    setLatency(null);
    setError(null);
    const tick = () => {
      const started = Date.now();
      AdminService.GlobalStatus(connID)
        .then((s) => {
          if (!s) return;
          const now = Date.now();
          setSnap(s);
          setError(null);
          setLatency(now - started);
          setCheckedAt(new Date(now));
          if (prev.current) {
            const dq = s.questions - prev.current.q;
            const dt = (now - prev.current.at) / 1000;
            if (dt > 0 && dq >= 0) setQps(Math.round(dq / dt));
          }
          prev.current = { q: s.questions, at: now };
        })
        .catch((e) => {
          // Keep the last snapshot visible; the dot carries the bad news.
          setError(e instanceof Error ? e.message : String(e));
          setCheckedAt(new Date());
          setLatency(null);
          prev.current = null;
          setQps(null);
        });
    };
    tick();
    const t = setInterval(tick, POLL_MS);
    return () => clearInterval(t);
  }, [connID]);

  const conn = saved.find((c) => c.id === connID);
  const healthy = error === null && snap !== null;
  const details = [
    conn ? `${conn.name} — ${conn.user}@${conn.host}:${conn.port}` : connID,
    healthy ? `healthy · ${latency ?? "?"}ms roundtrip` : `unreachable: ${error ?? "no data yet"}`,
    checkedAt ? `last check ${checkedAt.toLocaleTimeString()}` : "",
  ]
    .filter(Boolean)
    .join(" · ");

  const item = (tip: string, icon: ReactNode, text: string) => (
    <span className="flex cursor-default items-center gap-1.5" data-tip={tip}>
      {icon}
      {text}
    </span>
  );

  return (
    <footer className="flex h-7 shrink-0 items-center gap-4 border-t px-3 font-mono text-[11px] text-muted-foreground">
      <span className="flex cursor-default items-center gap-1.5" data-tip={details}>
        <span
          className={
            "h-2 w-2 rounded-full " +
            (healthy ? "bg-success" : "bg-destructive animate-pulse")
          }
        />
        {healthy ? `${latency ?? "–"}ms` : "offline"}
      </span>
      {snap && (
        <>
          {item("MySQL Server Version", <Database className="h-3 w-3" />, snap.version)}
          {item("Uptime", <Clock className="h-3 w-3" />, fmtUptime(snap.uptimeSeconds))}
          {item(
            "Threads Running / Connected",
            <Cpu className="h-3 w-3" />,
            `${snap.threadsRunning}/${snap.threadsConnected}`,
          )}
          {qps !== null &&
            item("Queries per Second", <Activity className="h-3 w-3" />, String(qps))}
        </>
      )}
      {error && <span className="truncate text-destructive">{error}</span>}
    </footer>
  );
}

function fmtUptime(up: number): string {
  return up >= 86400
    ? `${Math.floor(up / 86400)}d ${Math.floor((up % 86400) / 3600)}h`
    : up >= 3600
      ? `${Math.floor(up / 3600)}h ${Math.floor((up % 3600) / 60)}m`
      : `${Math.floor(up / 60)}m`;
}
