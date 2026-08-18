import { useEffect, useState } from "react";
import { AdminService } from "@/lib/api";
import type { UserRow } from "@/lib/api";
import { useApp } from "@/store";

// Users and Privileges panel: account list + SHOW GRANTS detail. Read-only
// in MVP (SPEC.md); management actions are phase 2.

type Props = { connID: string; active: boolean };

export function UsersView({ connID, active }: Props) {
  const [users, setUsers] = useState<UserRow[]>([]);
  const [selected, setSelected] = useState<UserRow | null>(null);
  const [grants, setGrants] = useState<string[]>([]);
  const [error, setError] = useState<string | null>(null);

  const connected = useApp((s) => s.openIDs.includes(connID));
  useEffect(() => {
    if (!active || !connected) return;
    AdminService.Users(connID)
      .then((list) => setUsers((list ?? []).filter((u): u is UserRow => u !== null)))
      .catch((e) => setError(e instanceof Error ? e.message : String(e)));
  }, [connID, active, connected]);

  useEffect(() => {
    if (!selected) return;
    AdminService.Grants(connID, selected.user, selected.host)
      .then((g) => setGrants((g ?? []).filter((s): s is string => s !== null)))
      .catch((e) => setError(e instanceof Error ? e.message : String(e)));
  }, [connID, selected]);

  return (
    <div className="flex h-full">
      <div className="w-72 shrink-0 overflow-auto border-r">
        <div className="border-b p-2 text-xs font-medium">
          Users {users.length > 0 && `(${users.length})`}
        </div>
        {users.map((u) => (
          <button
            key={`${u.user}@${u.host}`}
            type="button"
            onClick={() => setSelected(u)}
            className={
              "block w-full px-3 py-1.5 text-left font-mono text-xs hover:bg-muted/50 " +
              (selected?.user === u.user && selected?.host === u.host ? "bg-muted" : "")
            }
          >
            <span className="text-foreground">{u.user}</span>
            <span className="text-muted-foreground">@{u.host}</span>
            {u.locked && <span className="ml-2 text-warning">locked</span>}
            <div className="text-[10px] text-muted-foreground">{u.plugin}</div>
          </button>
        ))}
      </div>
      <div className="min-w-0 flex-1 overflow-auto p-3">
        {error && (
          <pre className="mb-2 whitespace-pre-wrap rounded-md border border-destructive/50 bg-destructive/10 p-2 font-mono text-xs text-destructive">
            {error}
          </pre>
        )}
        {!selected && <p className="text-sm text-muted-foreground">Select an account to see its grants.</p>}
        {selected && (
          <>
            <div className="mb-2 font-mono text-sm">
              {selected.user}
              <span className="text-muted-foreground">@{selected.host}</span>
            </div>
            <div className="flex flex-col gap-1">
              {grants.map((g, i) => (
                <code key={i} className="rounded bg-muted/40 px-2 py-1 font-mono text-xs">
                  {g}
                </code>
              ))}
            </div>
          </>
        )}
      </div>
    </div>
  );
}
