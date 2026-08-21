import { useEffect, useState } from "react";
import { ConnectionService } from "@/lib/api";
import type {
  SavedConn,
  TeleportStatus,
  TeleportDB,
  SSHAgentStatus,
  SSHBrowse,
  SSHFile,
} from "@/lib/api";
import { useApp, connHue } from "@/store";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
  DialogTrigger,
} from "@/components/ui/dialog";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";

// Connection manager, Workbench-style: a connection-method dropdown decides
// which fields exist. Passwords go straight to the backend Save (keyring /
// dev fallback) — never into app state.

const METHODS: [string, string][] = [
  ["tcp", "Standard (TCP/IP)"],
  ["ssh", "Standard (TCP/IP) over SSH"],
  ["teleport", "Teleport"],
];

// Auto-accent as #rrggbb for the color input (hsl h, s=0.6, l=0.5).
function hueToHex(h: number): string {
  const f = (n: number) => {
    const k = (n + h / 30) % 12;
    const c = 0.5 - 0.3 * Math.max(-1, Math.min(k - 3, 9 - k, 1));
    return Math.round(255 * c)
      .toString(16)
      .padStart(2, "0");
  };
  return `#${f(0)}${f(8)}${f(4)}`;
}

// "in 9h 51m" / "expired 2h ago" for the tsh session expiry.
function relTime(iso: string): { text: string; msLeft: number } {
  const ms = new Date(iso).getTime() - Date.now();
  const abs = Math.abs(ms);
  const h = Math.floor(abs / 3_600_000);
  const m = Math.floor((abs % 3_600_000) / 60_000);
  const span = h > 0 ? `${h}h ${m}m` : `${m}m`;
  return { text: ms >= 0 ? `in ${span}` : `expired ${span} ago`, msLeft: ms };
}

// Reopen the browser where the current value points (its directory), so
// editing an existing key does not start over at ~/.ssh.
function keyDir(keyFile: string): string {
  const i = keyFile.lastIndexOf("/");
  return i > 0 ? keyFile.slice(0, i) : "";
}

const empty = (): SavedConn => ({
  id: "",
  name: "",
  color: "",
  method: "tcp",
  host: "",
  port: 3306,
  user: "",
  database: "",
  tlsMode: "preferred",
  sshHost: "",
  sshPort: 22,
  sshUser: "",
  sshKeyFile: "",
  teleport: false,
  teleportDb: "",
});

// One line per hop: an ssh target plus its jump host does not fit on a single
// row without truncating the part that tells the two connections apart.
function summary(c: SavedConn): string[] {
  switch (c.method || (c.teleport ? "teleport" : "tcp")) {
    case "teleport":
      return [`${c.user}@tsh:${c.teleportDb}`];
    case "ssh":
      return [
        `${c.user}@${c.host}:${c.port}`,
        `via ssh ${c.sshUser ? `${c.sshUser}@` : ""}${c.sshHost}${c.sshPort && c.sshPort !== 22 ? `:${c.sshPort}` : ""}`,
      ];
    default:
      return [`${c.user}@${c.host}:${c.port}`];
  }
}

export function ConnDialog() {
  const { saved, openIDs, refreshSaved, openConn, closeConn } = useApp();
  const [open, setOpen] = useState(false);
  const [editing, setEditing] = useState<SavedConn | null>(null);
  const [password, setPassword] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const [tstatus, setTstatus] = useState<TeleportStatus | null>(null);
  const [tdbs, setTdbs] = useState<TeleportDB[] | null>(null);
  const [agent, setAgent] = useState<SSHAgentStatus | null>(null);
  const [keyBrowse, setKeyBrowse] = useState<SSHBrowse | null>(null);

  const method = editing?.method || "tcp";

  // Errors clear on the next action anyway — also auto-dismiss so a stale
  // one doesn't sit under the form while the user fixes the cause.
  useEffect(() => {
    if (!error) return;
    const t = setTimeout(() => setError(null), 6000);
    return () => clearTimeout(t);
  }, [error]);

  // tsh status is a fast local check; run it while the Teleport method is up.
  useEffect(() => {
    if (method !== "teleport") {
      setTstatus(null);
      setTdbs(null);
      return;
    }
    ConnectionService.TeleportStatus()
      .then(setTstatus)
      .catch(() => setTstatus(null));
  }, [method]);

  // Agent availability decides whether a key file is optional or mandatory,
  // so check it while the SSH method is up — same pattern as tsh status.
  useEffect(() => {
    if (method !== "ssh") {
      setAgent(null);
      setKeyBrowse(null);
      return;
    }
    ConnectionService.SSHAgentStatus()
      .then(setAgent)
      .catch(() => setAgent(null));
  }, [method]);

  const browseKeys = async (dir: string) => {
    setError(null);
    try {
      setKeyBrowse(await ConnectionService.SSHBrowse(dir));
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  };

  const browseTeleport = async () => {
    setError(null);
    try {
      const list = await ConnectionService.TeleportDatabases();
      setTdbs((list ?? []).filter((d): d is TeleportDB => d !== null));
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  };

  const pickResource = (d: TeleportDB) => {
    if (!editing) return;
    const allowed = (d.users ?? []).filter((u): u is string => u !== null && u !== "*");
    // One concrete allowed user → fill it in; more → the chips below decide.
    const user = editing.user || (allowed.length === 1 ? allowed[0] : "");
    setEditing({ ...editing, teleportDb: d.name, user });
  };

  const allowedUsers =
    method === "teleport" && editing
      ? ((tdbs?.find((d) => d.name === editing.teleportDb)?.users ?? []).filter(
          (u): u is string => u !== null && u !== "*",
        ) ?? [])
      : [];

  const save = async () => {
    if (!editing) return;
    setError(null);
    try {
      await ConnectionService.Save(
        { ...editing, teleport: method === "teleport" },
        password,
      );
      setEditing(null);
      setPassword("");
      await refreshSaved();
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  };

  const connect = async (id: string) => {
    setError(null);
    setBusy(id);
    try {
      await openConn(id);
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(null);
    }
  };

  const remove = async (id: string) => {
    setError(null);
    try {
      await ConnectionService.Delete(id);
      await refreshSaved();
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  };

  const field = (
    label: string,
    value: string | number,
    onChange: (v: string) => void,
    opts?: { type?: string; placeholder?: string },
  ) => (
    <div className="grid grid-cols-3 items-center gap-2">
      <Label className="text-right text-xs">{label}</Label>
      <Input
        className="col-span-2 h-8 font-mono text-xs"
        type={opts?.type ?? "text"}
        placeholder={opts?.placeholder}
        value={value}
        onChange={(e) => onChange(e.target.value)}
      />
    </div>
  );

  const note = (text: string) => (
    <p className="ml-[33%] pl-2 pr-2 text-xs text-muted-foreground">{text}</p>
  );

  // Closing the dialog any way (✕, Escape, overlay) discards the in-progress
  // edit, so reopening starts at the connection list — same as Cancel.
  const onOpenChange = (o: boolean) => {
    setOpen(o);
    if (!o) {
      setEditing(null);
      setPassword("");
      setError(null);
      setTdbs(null);
      setKeyBrowse(null);
    }
  };

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogTrigger className="inline-flex h-8 items-center rounded-md border bg-background px-3 text-xs font-medium hover:bg-muted">
        Connections
      </DialogTrigger>
      <DialogContent className="sm:max-w-2xl">
        <DialogHeader>
          <DialogTitle>Connections</DialogTitle>
        </DialogHeader>

        {!editing && (
          <div className="flex max-h-[calc(100dvh-10rem)] flex-col gap-2 overflow-y-auto pr-1">
            {saved.map((c) => (
              <div key={c.id} className="flex items-center gap-2 rounded-md border p-2">
                <span
                  className="h-2.5 w-2.5 shrink-0 rounded-full"
                  style={{ background: c.color || `hsl(${connHue(c.id)} 60% 50%)` }}
                />
                <div className="min-w-0 flex-1">
                  <div className="truncate text-sm">{c.name}</div>
                  {summary(c).map((line) => (
                    <div key={line} className="truncate font-mono text-xs text-muted-foreground">
                      {line}
                    </div>
                  ))}
                </div>
                {openIDs.includes(c.id) ? (
                  <Button size="sm" variant="outline" onClick={() => closeConn(c.id)}>
                    Disconnect
                  </Button>
                ) : (
                  <Button size="sm" disabled={busy === c.id} onClick={() => void connect(c.id)}>
                    {busy === c.id ? "Connecting…" : "Connect"}
                  </Button>
                )}
                <Button size="sm" variant="ghost" onClick={() => setEditing({ ...c })}>
                  Edit
                </Button>
                <Button
                  size="sm"
                  variant="ghost"
                  className="text-destructive"
                  onClick={() => void remove(c.id)}
                >
                  ✕
                </Button>
              </div>
            ))}
            <Button size="sm" variant="secondary" onClick={() => setEditing(empty())}>
              New Connection
            </Button>
          </div>
        )}

        {editing && (
          <div className="flex max-h-[calc(100dvh-10rem)] flex-col gap-2 overflow-y-auto pr-1">
            {field("Name", editing.name, (v) => setEditing({ ...editing, name: v }))}

            <div className="grid grid-cols-3 items-center gap-2">
              <Label className="text-right text-xs">Color</Label>
              <div className="col-span-2 flex items-center gap-2">
                <input
                  type="color"
                  className="h-8 w-14 cursor-pointer rounded-md border bg-transparent p-0.5"
                  value={editing.color || hueToHex(connHue(editing.id))}
                  onChange={(e) => setEditing({ ...editing, color: e.target.value })}
                />
                {editing.color ? (
                  <Button size="xs" variant="ghost" onClick={() => setEditing({ ...editing, color: "" })}>
                    reset to auto
                  </Button>
                ) : (
                  <span className="text-xs text-muted-foreground">auto</span>
                )}
              </div>
            </div>

            <div className="grid grid-cols-3 items-center gap-2">
              <Label className="text-right text-xs">Method</Label>
              <Select
                value={method}
                onValueChange={(v) => setEditing({ ...editing, method: v ?? "tcp" })}
              >
                <SelectTrigger className="col-span-2 h-8 text-xs">
                  <SelectValue>
                    {METHODS.find(([k]) => k === method)?.[1] ?? method}
                  </SelectValue>
                </SelectTrigger>
                <SelectContent className="text-xs">
                  {METHODS.map(([k, label]) => (
                    <SelectItem key={k} value={k}>
                      {label}
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
            </div>

            {method === "ssh" && (
              <>
                {field("SSH Host", editing.sshHost, (v) => setEditing({ ...editing, sshHost: v }))}
                {field("SSH Port", editing.sshPort || 22, (v) =>
                  setEditing({ ...editing, sshPort: Number(v) || 22 }), { type: "number" })}
                {field("SSH User", editing.sshUser, (v) => setEditing({ ...editing, sshUser: v }))}
                <div className="grid grid-cols-3 items-center gap-2">
                  <Label className="text-right text-xs">SSH Key File</Label>
                  <div className="col-span-2 flex items-center gap-2">
                    <Input
                      className="h-8 flex-1 font-mono text-xs"
                      placeholder={
                        agent?.found
                          ? "optional — the agent holds a usable key"
                          : "path to a private key"
                      }
                      value={editing.sshKeyFile}
                      onChange={(e) => setEditing({ ...editing, sshKeyFile: e.target.value })}
                    />
                    <Button
                      size="sm"
                      variant="outline"
                      className="h-8"
                      onClick={() => void browseKeys(keyDir(editing.sshKeyFile))}
                    >
                      Browse
                    </Button>
                  </div>
                </div>
                {keyBrowse && (
                  <div className="ml-[33%] rounded-md border">
                    <div className="flex items-center gap-2 border-b px-2 py-1">
                      <span className="truncate font-mono text-xs text-muted-foreground">
                        {keyBrowse.dir}
                      </span>
                      <Button
                        size="xs"
                        variant="ghost"
                        className="ml-auto"
                        onClick={() => setKeyBrowse(null)}
                      >
                        close
                      </Button>
                    </div>
                    <div className="max-h-36 overflow-auto">
                      {keyBrowse.parent && (
                        <button
                          type="button"
                          className="flex w-full items-baseline gap-2 px-2 py-1 text-left text-xs hover:bg-muted/50"
                          onClick={() => void browseKeys(keyBrowse.parent)}
                        >
                          <span className="font-mono">../</span>
                        </button>
                      )}
                      {(keyBrowse.files ?? []).length === 0 && !keyBrowse.parent && (
                        <p className="p-2 text-xs text-muted-foreground">Nothing here.</p>
                      )}
                      {(keyBrowse.files ?? [])
                        .filter((f): f is SSHFile => f !== null)
                        .map((f) => (
                          <button
                            key={f.path}
                            type="button"
                            className="flex w-full items-baseline gap-2 px-2 py-1 text-left text-xs hover:bg-muted/50"
                            title={f.path}
                            onClick={() => {
                              if (f.dir) {
                                void browseKeys(f.path);
                                return;
                              }
                              setEditing({ ...editing, sshKeyFile: f.path });
                              setKeyBrowse(null);
                            }}
                          >
                            <span className="font-mono">
                              {f.name}
                              {f.dir ? "/" : ""}
                            </span>
                          </button>
                        ))}
                    </div>
                  </div>
                )}
                {agent === null && note("checking ssh-agent…")}
                {agent?.found &&
                  note(
                    agent.keys > 0
                      ? `ssh-agent: ${agent.keys} key${agent.keys === 1 ? "" : "s"} (${agent.socket})`
                      : agent.detail,
                  )}
                {agent && !agent.found && note(agent.detail)}
                {field("MySQL Host", editing.host, (v) => setEditing({ ...editing, host: v }), {
                  placeholder: "127.0.0.1 — as seen from the SSH host",
                })}
                {field("MySQL Port", editing.port, (v) =>
                  setEditing({ ...editing, port: Number(v) || 0 }), { type: "number" })}
              </>
            )}

            {method === "tcp" && (
              <>
                {field("Host", editing.host, (v) => setEditing({ ...editing, host: v }))}
                {field("Port", editing.port, (v) =>
                  setEditing({ ...editing, port: Number(v) || 0 }), { type: "number" })}
              </>
            )}

            {method === "teleport" && (
              <>
                <div className="grid grid-cols-3 items-center gap-2">
                  <Label className="text-right text-xs">Resource</Label>
                  <div className="col-span-2 flex items-center gap-2">
                    <Input
                      placeholder="teleport db resource"
                      className="h-8 flex-1 font-mono text-xs"
                      value={editing.teleportDb}
                      onChange={(e) => setEditing({ ...editing, teleportDb: e.target.value })}
                    />
                    <Button size="sm" variant="outline" className="h-8" onClick={() => void browseTeleport()}>
                      Browse
                    </Button>
                  </div>
                </div>
                {tdbs && (
                  <div className="ml-[33%] max-h-36 overflow-auto rounded-md border">
                    {tdbs.length === 0 && (
                      <p className="p-2 text-xs text-muted-foreground">No database resources visible.</p>
                    )}
                    {tdbs.map((d) => (
                      <button
                        key={d.name}
                        type="button"
                        disabled={d.protocol !== "mysql"}
                        className="flex w-full items-baseline gap-2 px-2 py-1 text-left text-xs hover:bg-muted/50 disabled:cursor-not-allowed disabled:opacity-40"
                        title={d.protocol === "mysql" ? d.description : `${d.protocol} — not MySQL`}
                        onClick={() => pickResource(d)}
                      >
                        <span className="font-mono">{d.name}</span>
                        <span className="text-muted-foreground">{d.protocol}</span>
                      </button>
                    ))}
                  </div>
                )}
                {tstatus === null && note("checking tsh…")}
                {tstatus && !tstatus.loggedIn && note(tstatus.detail)}
                {tstatus?.loggedIn && (
                  // Structured session card from `tsh status -f json` —
                  // beats eyeballing the raw text dump.
                  <div className="ml-[33%] space-y-1.5 rounded-md border p-2 text-xs">
                    <div className="flex items-center gap-1.5">
                      <span
                        className={
                          "h-2 w-2 shrink-0 rounded-full " +
                          (tstatus.validUntil && relTime(tstatus.validUntil).msLeft < 30 * 60_000
                            ? "bg-warning"
                            : "bg-success")
                        }
                      />
                      <span className="truncate font-mono">
                        {tstatus.username}
                        <span className="text-muted-foreground"> @ {tstatus.cluster}</span>
                      </span>
                    </div>
                    {tstatus.validUntil && (
                      <div className="text-muted-foreground">
                        session expires {relTime(tstatus.validUntil).text} (
                        {new Date(tstatus.validUntil).toLocaleTimeString([], {
                          hour: "2-digit",
                          minute: "2-digit",
                        })}
                        )
                      </div>
                    )}
                    {(tstatus.dbUsers?.length ?? 0) > 0 && (
                      <div className="flex flex-wrap items-center gap-1">
                        <span className="text-muted-foreground">db users</span>
                        {tstatus.dbUsers!.map((u) => (
                          <button
                            key={u}
                            type="button"
                            data-tip="Use as DB user"
                            className={
                              "rounded-full border px-2 py-0.5 font-mono " +
                              (editing.user === u
                                ? "border-primary text-foreground"
                                : "text-muted-foreground hover:bg-muted/50")
                            }
                            onClick={() => setEditing({ ...editing, user: u })}
                          >
                            {u}
                          </button>
                        ))}
                      </div>
                    )}
                    {(tstatus.databases?.length ?? 0) > 0 && (
                      <div className="text-muted-foreground">
                        active db certs:{" "}
                        <span className="font-mono text-foreground">
                          {tstatus.databases!.join(", ")}
                        </span>
                      </div>
                    )}
                  </div>
                )}
              </>
            )}

            {field("User", editing.user, (v) => setEditing({ ...editing, user: v }))}
            {allowedUsers.length > 1 && (
              <div className="ml-[33%] flex flex-wrap gap-1 pl-2">
                {allowedUsers.map((u) => (
                  <button
                    key={u}
                    type="button"
                    className={
                      "rounded-full border px-2 py-0.5 font-mono text-xs " +
                      (editing.user === u
                        ? "border-primary text-foreground"
                        : "text-muted-foreground hover:bg-muted/50")
                    }
                    onClick={() => setEditing({ ...editing, user: u })}
                  >
                    {u}
                  </button>
                ))}
              </div>
            )}
            {field("Database", editing.database, (v) => setEditing({ ...editing, database: v }), {
              placeholder: method === "teleport" ? "required by most teleport db configs" : "optional default schema",
            })}
            {field("Password", password, setPassword, { type: "password" })}
            {method === "teleport" &&
              note("usually empty — the tunnel authenticates with your tsh certs")}

            {method === "tcp" && (
              <div className="grid grid-cols-3 items-center gap-2">
                <Label className="text-right text-xs">TLS</Label>
                <Select
                  value={editing.tlsMode}
                  onValueChange={(v) => setEditing({ ...editing, tlsMode: v ?? "preferred" })}
                >
                  <SelectTrigger className="col-span-2 h-8 text-xs">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="preferred">preferred</SelectItem>
                    <SelectItem value="required">required</SelectItem>
                    <SelectItem value="disabled">disabled</SelectItem>
                  </SelectContent>
                </Select>
              </div>
            )}

            <div className="mt-2 flex justify-end gap-2">
              <Button size="sm" variant="ghost" onClick={() => setEditing(null)}>
                Cancel
              </Button>
              <Button size="sm" onClick={() => void save()}>
                Save
              </Button>
            </div>
          </div>
        )}

        {error && (
          <pre className="whitespace-pre-wrap rounded-md border border-destructive/50 bg-destructive/10 p-2 font-mono text-xs text-destructive">
            {error}
          </pre>
        )}
      </DialogContent>
    </Dialog>
  );
}
