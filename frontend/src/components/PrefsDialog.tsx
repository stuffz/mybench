import { useEffect, useState } from "react";
import { Info, Settings } from "lucide-react";
import { MCPService, QueryService } from "@/lib/api";
import type { MCPStatus } from "@/lib/api";
import { Button } from "@/components/ui/button";
import {
  Dialog,
  DialogContent,
  DialogDescription,
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
import { Switch } from "@/components/ui/switch";
import { useApp } from "@/store";
import {
  APP_THEMES,
  EDITOR_THEMES,
  UI_FONTS,
  MONO_FONTS,
  type Prefs,
} from "@/lib/prefs";

// Preferences: themes for the app chrome and the SQL editor, font stacks,
// and font-size sliders. Everything applies live and persists with the
// workspace.

function Row({
  label,
  tip,
  children,
}: {
  label: string;
  tip?: string;
  children: React.ReactNode;
}) {
  return (
    <div className="grid grid-cols-[11rem_1fr] items-center gap-3 text-xs">
      {tip ? (
        <span
          className="flex w-fit items-center gap-1 text-muted-foreground"
          data-tip={tip}
          data-tip-wide=""
        >
          {label}
          <Info className="h-3 w-3 shrink-0 opacity-60" />
        </span>
      ) : (
        <span className="text-muted-foreground">{label}</span>
      )}
      {children}
    </div>
  );
}

function PrefSelect<K extends keyof Prefs>({
  pref,
  options,
}: {
  pref: K;
  options: [Prefs[K] & string, string][];
}) {
  const value = useApp((s) => s.prefs[pref]) as string;
  const setPrefs = useApp((s) => s.setPrefs);
  const label = options.find(([v]) => v === value)?.[1] ?? value;
  return (
    <Select value={value} onValueChange={(v) => setPrefs({ [pref]: v } as Partial<Prefs>)}>
      <SelectTrigger className="h-8 w-full text-xs">
        <SelectValue>
          <span className="truncate">{label}</span>
        </SelectValue>
      </SelectTrigger>
      <SelectContent className="text-xs">
        {options.map(([v, label]) => (
          <SelectItem key={v} value={v}>
            {label}
          </SelectItem>
        ))}
      </SelectContent>
    </Select>
  );
}

function PrefSlider({
  pref,
  min,
  max,
  unit = "px",
}: {
  pref: "uiFontSize" | "editorFontSize" | "tabSize";
  min: number;
  max: number;
  unit?: string;
}) {
  const value = useApp((s) => s.prefs[pref]);
  const setPrefs = useApp((s) => s.setPrefs);
  return (
    <div className="flex items-center gap-2">
      <input
        type="range"
        min={min}
        max={max}
        step={1}
        value={value}
        className="h-1.5 flex-1 cursor-pointer appearance-none rounded-full bg-muted-foreground/30 accent-primary"
        onChange={(e) => setPrefs({ [pref]: Number(e.target.value) } as Partial<Prefs>)}
      />
      <span className="w-10 text-right font-mono tabular-nums">
        {value}
        {unit}
      </span>
    </div>
  );
}

function HideDefaultDBsSwitch() {
  const value = useApp((s) => s.prefs.hideDefaultDBs);
  const setPrefs = useApp((s) => s.setPrefs);
  return <Switch checked={value} onCheckedChange={(v) => setPrefs({ hideDefaultDBs: v })} />;
}

function fmtBytes(n: number): string {
  if (n >= 1024 * 1024) return (n / (1024 * 1024)).toPrecision(3) + " MB";
  if (n >= 1024) return (n / 1024).toPrecision(3) + " KB";
  return n + " B";
}

// Row + input for history retention; the tooltip includes live stats about
// how much history is currently stored (fetched when the dialog opens).
function HistoryKeepRow() {
  const value = useApp((s) => s.prefs.historyKeep);
  const setPrefs = useApp((s) => s.setPrefs);
  const [stats, setStats] = useState("");
  useEffect(() => {
    QueryService.HistoryStats()
      .then((s) => {
        if (s) setStats(` Currently storing ${s.count.toLocaleString()} statements (${fmtBytes(s.bytes)}).`);
      })
      .catch(() => setStats(""));
  }, []);
  return (
    <Row
      label="History Per Server"
      tip={`Newest executed statements kept per server; older entries are pruned.${stats}`}
    >
      <input
        type="number"
        min={100}
        step={1000}
        value={value}
        className="h-8 w-32 rounded-md border bg-transparent px-2 font-mono text-xs outline-none focus:border-ring"
        onChange={(e) => setPrefs({ historyKeep: Math.max(100, Number(e.target.value) || 100) })}
      />
    </Row>
  );
}

// Read-only MCP endpoint controls. Backed by MCPService directly (settings
// live backend-side in SQLite, not in the prefs blob).
function MCPSection() {
  const [status, setStatus] = useState<MCPStatus | null>(null);
  const [port, setPort] = useState<number | null>(null);
  const [copied, setCopied] = useState("");

  useEffect(() => {
    MCPService.Status()
      .then((s) => {
        setStatus(s);
        setPort(s?.port ?? null);
      })
      .catch(() => undefined);
  }, []);

  const configure = (enabled: boolean, p: number) => {
    MCPService.Configure(enabled, p)
      .then((s) => {
        setStatus(s);
        setPort(s?.port ?? null);
      })
      .catch(() => undefined);
  };

  const copy = (what: string, value: string) => {
    void navigator.clipboard.writeText(value).then(() => {
      setCopied(what);
      setTimeout(() => setCopied(""), 1500);
    });
  };

  if (!status) return null;
  return (
    <>
      <Row
        label="MCP Server"
        tip="Read-only Model Context Protocol endpoint for AI agents: schema introspection plus gated SELECT/SHOW/EXPLAIN through your OPEN connections (tunnels included, credentials never exposed). Loopback-only with bearer-token auth; every agent query is recorded in Query History."
      >
        <Switch
          checked={status.enabled}
          onCheckedChange={(v) => configure(v, port ?? status.port)}
        />
      </Row>
      {status.enabled && (
        <>
          <Row label="MCP Port">
            <input
              type="number"
              min={1}
              max={65535}
              value={port ?? status.port}
              className="h-8 w-32 rounded-md border bg-transparent px-2 font-mono text-xs outline-none focus:border-ring"
              onChange={(e) => setPort(Number(e.target.value) || status.port)}
              onBlur={() => port && port !== status.port && configure(true, port)}
              onKeyDown={(e) => {
                if (e.key === "Enter" && port) configure(true, port);
              }}
            />
          </Row>
          <Row label="MCP Endpoint">
            <div className="flex min-w-0 items-center gap-2">
              <code className="truncate font-mono text-xs text-muted-foreground">{status.url}</code>
              <Button size="xs" variant="outline" onClick={() => copy("url", status.url)}>
                {copied === "url" ? "Copied" : "Copy URL"}
              </Button>
              <Button size="xs" variant="outline" onClick={() => copy("token", status.token)}>
                {copied === "token" ? "Copied" : "Copy Token"}
              </Button>
              <Button
                size="xs"
                variant="ghost"
                data-tip="Invalidates the current token"
                onClick={() => void MCPService.RegenerateToken().then(setStatus).catch(() => undefined)}
              >
                Regenerate
              </Button>
            </div>
          </Row>
          {status.error && (
            <p className="text-xs text-destructive">MCP: {status.error}</p>
          )}
        </>
      )}
    </>
  );
}

function MaxRowsInput() {
  const value = useApp((s) => s.prefs.maxRows);
  const setPrefs = useApp((s) => s.setPrefs);
  return (
    <input
      type="number"
      min={100}
      step={1000}
      value={value}
      className="h-8 w-32 rounded-md border bg-transparent px-2 font-mono text-xs outline-none focus:border-ring"
      onChange={(e) => setPrefs({ maxRows: Math.max(100, Number(e.target.value) || 100) })}
    />
  );
}

export function PrefsDialog() {
  return (
    <Dialog>
      <DialogTrigger
        className="flex items-center gap-1.5 rounded-md px-2 py-1 text-xs text-muted-foreground hover:bg-muted/50 hover:text-foreground"
        title="Preferences"
      >
        <Settings className="h-3.5 w-3.5" />
        Preferences
      </DialogTrigger>
      <DialogContent className="sm:max-w-xl">
        <DialogHeader>
          <DialogTitle>Preferences</DialogTitle>
          <DialogDescription>
            Applied immediately, saved with your workspace.
          </DialogDescription>
        </DialogHeader>
        <div className="flex flex-col gap-3">
          <Row label="App Theme">
            <PrefSelect pref="appTheme" options={APP_THEMES} />
          </Row>
          <Row label="Editor Theme">
            <PrefSelect pref="editorTheme" options={EDITOR_THEMES} />
          </Row>
          <Row label="UI Font">
            <PrefSelect pref="uiFont" options={UI_FONTS} />
          </Row>
          <Row label="Editor Font">
            <PrefSelect pref="monoFont" options={MONO_FONTS} />
          </Row>
          <Row label="UI Font Size">
            <PrefSlider pref="uiFontSize" min={12} max={20} />
          </Row>
          <Row label="Editor Font Size">
            <PrefSlider pref="editorFontSize" min={10} max={22} />
          </Row>
          <Row label="Tab Size">
            <PrefSlider pref="tabSize" min={2} max={8} unit="" />
          </Row>
          <Row
            label="Hide Default Databases"
            tip="Hides information_schema, performance_schema, mysql and sys in the schema tree"
          >
            <HideDefaultDBsSwitch />
          </Row>
          <HistoryKeepRow />
          <Row
            label="Absolute Max Rows"
            tip="Hard ceiling on rows fetched into memory — the cap “Fetch All” uses. Only limits fetching; the SQL is never modified and the server still runs the full query."
          >
            <MaxRowsInput />
          </Row>
          <MCPSection />
        </div>
      </DialogContent>
    </Dialog>
  );
}
