import { useCallback, useEffect, useState } from "react";
import { AdminService } from "@/lib/api";
import type { Process } from "@/lib/api";
import { Button } from "@/components/ui/button";
import { Switch } from "@/components/ui/switch";
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";
import { ZapOff, Unplug, TriangleAlert, Copy, Check } from "lucide-react";
import { useApp } from "@/store";

// Client Connections panel: processlist + kill (behind a confirm dialog —
// both kills are destructive). Auto-refresh only ticks while the tab is
// visible (idle-CPU rule).

type Props = { connID: string; active: boolean };

type PendingKill = { proc: Process; queryOnly: boolean };

export function ProcesslistView({ connID, active }: Props) {
  const [procs, setProcs] = useState<Process[]>([]);
  const [auto, setAuto] = useState(true);
  const [confirm, setConfirm] = useState<PendingKill | null>(null);
  const [error, setError] = useState<string | null>(null);
  // Row whose statement was just copied — brief check-mark feedback.
  const [copiedId, setCopiedId] = useState<number | null>(null);

  const copyInfo = (p: Process) => {
    void navigator.clipboard.writeText(p.info).then(() => {
      setCopiedId(p.id);
      setTimeout(() => setCopiedId((cur) => (cur === p.id ? null : cur)), 1500);
    });
  };

  const refresh = useCallback(async () => {
    try {
      const list = await AdminService.Processlist(connID);
      setProcs((list ?? []).filter((p): p is Process => p !== null));
      setError(null);
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }, [connID]);

  const connected = useApp((s) => s.openIDs.includes(connID));
  useEffect(() => {
    if (!active || !connected) return;
    void refresh();
    if (!auto) return;
    const t = setInterval(() => void refresh(), 2000);
    return () => clearInterval(t);
  }, [active, connected, auto, refresh]);

  const kill = async (id: number, queryOnly: boolean) => {
    setConfirm(null);
    try {
      await AdminService.Kill(connID, id, queryOnly);
      await refresh();
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  };

  return (
    <div className="flex h-full flex-col">
      <div className="flex h-[calc(2.5rem+1px)] shrink-0 items-center gap-3 border-b px-3 text-xs">
        <span className="font-medium">Client Connections</span>
        <label className="flex items-center gap-1.5 text-muted-foreground">
          <Switch checked={auto} onCheckedChange={setAuto} /> Auto-refresh
        </label>
        <Button size="xs" variant="outline" onClick={() => void refresh()}>
          Refresh
        </Button>
        <span className="text-muted-foreground">{procs.length} threads</span>
      </div>
      {error && (
        <pre className="m-2 whitespace-pre-wrap rounded-md border border-destructive/50 bg-destructive/10 p-2 font-mono text-xs text-destructive">
          {error}
        </pre>
      )}
      <div className="min-h-0 flex-1 overflow-auto font-mono text-xs">
        <table className="w-full">
          <thead className="sticky top-0 bg-background text-left text-muted-foreground">
            <tr>
              {["Id", "User", "Host", "DB", "Command", "Time", "State", "Info", ""].map((h) => (
                <th key={h} className="whitespace-nowrap px-2 py-1 font-normal">
                  {h}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {procs.map((p) => (
              <tr key={p.id} className="border-t border-border/40 hover:bg-muted/30">
                <td className="px-2 py-1">{p.id}</td>
                <td className="px-2 py-1">{p.user}</td>
                <td className="px-2 py-1">{p.host}</td>
                <td className="px-2 py-1">{p.db}</td>
                <td className="px-2 py-1">{p.command}</td>
                <td className="px-2 py-1 tabular-nums">{p.time}</td>
                <td className="max-w-56 truncate px-2 py-1" title={p.state}>
                  {p.state}
                </td>
                <td className="max-w-96 truncate px-2 py-1 text-muted-foreground" title={p.info}>
                  {p.info}
                </td>
                <td className="whitespace-nowrap px-2 py-1">
                  <button
                    type="button"
                    className={
                      "mr-1 rounded p-1 " +
                      (p.info
                        ? "text-muted-foreground hover:bg-muted/50 hover:text-foreground"
                        : "invisible")
                    }
                    data-tip="Copy the full running statement"
                    data-tip-right=""
                    onClick={() => copyInfo(p)}
                  >
                    {copiedId === p.id ? (
                      <Check className="h-3.5 w-3.5 text-success" />
                    ) : (
                      <Copy className="h-3.5 w-3.5" />
                    )}
                  </button>
                  <button
                    type="button"
                    className="mr-1 rounded p-1 text-warning hover:bg-warning/10"
                    data-tip="Kill query — stop the statement, keep the connection"
                    data-tip-right=""
                    onClick={() => setConfirm({ proc: p, queryOnly: true })}
                  >
                    <ZapOff className="h-3.5 w-3.5" />
                  </button>
                  <button
                    type="button"
                    className="rounded p-1 text-destructive hover:bg-destructive/10"
                    data-tip="Kill connection — disconnect this client"
                    data-tip-right=""
                    onClick={() => setConfirm({ proc: p, queryOnly: false })}
                  >
                    <Unplug className="h-3.5 w-3.5" />
                  </button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      <Dialog open={confirm !== null} onOpenChange={(open) => !open && setConfirm(null)}>
        <DialogContent className="sm:max-w-md">
          {confirm && (
            <>
              <DialogHeader>
                <DialogTitle className="flex items-center gap-2">
                  <TriangleAlert className="h-4 w-4 text-destructive" />
                  {confirm.queryOnly ? "Kill query?" : "Kill connection?"}
                </DialogTitle>
                <DialogDescription>
                  {confirm.queryOnly
                    ? "Stops the running statement on this thread; the client stays connected."
                    : "Terminates the client's connection; anything it is running is aborted."}
                </DialogDescription>
              </DialogHeader>
              <div className="rounded-md border bg-muted/20 p-2 font-mono text-xs">
                <div>
                  thread {confirm.proc.id} · {confirm.proc.user}@{confirm.proc.host}
                  {confirm.proc.db ? ` · ${confirm.proc.db}` : ""}
                </div>
                {confirm.proc.info && (
                  <div className="mt-1 max-h-24 overflow-auto whitespace-pre-wrap text-muted-foreground">
                    {confirm.proc.info}
                  </div>
                )}
              </div>
              <DialogFooter>
                <Button size="sm" variant="outline" onClick={() => setConfirm(null)}>
                  Cancel
                </Button>
                <Button
                  size="sm"
                  variant="destructive"
                  onClick={() => void kill(confirm.proc.id, confirm.queryOnly)}
                >
                  {confirm.queryOnly ? "Kill query" : "Kill connection"}
                </Button>
              </DialogFooter>
            </>
          )}
        </DialogContent>
      </Dialog>
    </div>
  );
}
