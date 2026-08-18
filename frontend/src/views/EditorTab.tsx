import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { AdminService, QueryService } from "@/lib/api";
import type { EditInfo, ResultState, Snippet } from "@/lib/api";
import { Button } from "@/components/ui/button";
import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu";
import { X } from "lucide-react";
import { Grid, type SortState, type StagedEdit } from "@/components/Grid";
import { PlanView } from "@/components/PlanView";
import { SqlEditor, type SqlEditorHandle } from "@/components/SqlEditor";
import { startDrag } from "@/lib/drag";
import { useApp } from "@/store";

// One editor tab: its own CodeMirror instance, its own dedicated backend
// session (keyed connID+tabID), its own result. Mounted for the tab's
// lifetime — editor undo history and results survive tab switches.

const POLL_MS = 250;

// Completion schema per connection, fetched once and shared between tabs.
// Failures are evicted so a fetch that raced the connection opening (restore)
// retries instead of leaving completion empty for the session.
const schemaCache = new Map<string, Promise<Record<string, string[]>>>();
function completionSchema(connID: string): Promise<Record<string, string[]>> {
  const cached = schemaCache.get(connID);
  if (cached) return cached;
  const p = AdminService.SchemaMap(connID)
    .then((m) => {
      const out: Record<string, string[]> = {};
      for (const [k, v] of Object.entries(m ?? {})) {
        out[k] = ((v ?? []) as (string | null)[]).filter((c): c is string => c !== null);
      }
      return out;
    })
    .catch((e) => {
      schemaCache.delete(connID);
      throw e;
    });
  schemaCache.set(connID, p);
  return p;
}

type Props = { tabID: string; connID: string; initialSQL?: string };

// A user statement that already starts with EXPLAIN is unwrapped before we
// re-wrap it, so "Explain" on an EXPLAIN buffer doesn't double up.
const STRIP_EXPLAIN = /^\s*explain\s+(analyze\s+)?(format\s*=\s*\w+\s+)?/i;

type PlanKind = "explain" | "analyze" | null;

export function EditorTab({ tabID, connID, initialSQL }: Props) {
  const editor = useRef<SqlEditorHandle>(null);
  const [schema, setSchema] = useState<Record<string, string[]> | null>(null);
  const [state, setState] = useState<ResultState | null>(null);
  const [sort, setSort] = useState<SortState>(null);
  const [version, setVersion] = useState(0);
  const [error, setError] = useState<string | null>(null);
  const [sessionReset, setSessionReset] = useState(false);
  const [csvPath, setCsvPath] = useState<string | null>(null);
  // Non-null while the current result is an EXPLAIN run; the grid area shows
  // the plan tree instead of rows until the next plain Run.
  const [planKind, setPlanKind] = useState<PlanKind>(null);
  const [planText, setPlanText] = useState<string | null>(null);
  // Explain Analyze EXECUTES the statement; non-read statements need a
  // second click within the arm window.
  const [armedAnalyze, setArmedAnalyze] = useState(false);
  const armTimer = useRef<number | null>(null);
  // Editable grid: whether/how this result maps back to one table, the
  // staged cell edits, and the UPDATE preview awaiting confirmation.
  const [editInfo, setEditInfo] = useState<EditInfo | null>(null);
  const [edits, setEdits] = useState<Map<string, StagedEdit>>(new Map());
  const [preview, setPreview] = useState<string[] | null>(null);
  const [applyNote, setApplyNote] = useState<string | null>(null);
  // Snippet library (global): loaded when the dropdown opens; save dialog
  // captures the current statement/selection under a name.
  const [snippets, setSnippets] = useState<Snippet[]>([]);
  const [snippetName, setSnippetName] = useState<string | null>(null); // non-null = save dialog open
  const [snippetSQL, setSnippetSQL] = useState("");
  // Editor pane height (splitter) — persisted on the tab in the workspace.
  const savedH = useApp((s) => s.tabs.find((t) => t.tabID === tabID)?.editorH);
  const setTabEditorH = useApp((s) => s.setTabEditorH);
  const editorH =
    savedH ?? Math.min(420, Math.max(160, Math.round(window.innerHeight * 0.34)));
  // Absolute row ceiling from preferences; applies to "Fetch All" too.
  const maxRowsPref = useApp((s) => s.prefs.maxRows);
  const stateRef = useRef<ResultState | null>(null);
  stateRef.current = state;

  const running = !!state && !state.done;

  const connected = useApp((s) => s.openIDs.includes(connID));
  useEffect(() => {
    if (!connected) return;
    completionSchema(connID)
      .then(setSchema)
      .catch(() => setSchema({}));
  }, [connID, connected]);

  // Document changes flow to the store so the workspace file stays current.
  const setTabSQL = useApp((s) => s.setTabSQL);

  useEffect(() => {
    if (!state || state.done) return;
    const id = state.resultId;
    const t = setInterval(() => {
      QueryService.State(id)
        .then((s) => s && setState(s))
        .catch((e) => {
          // A dead poll must not leave the tab stuck "running" forever —
          // that silently blocks every future run.
          clearInterval(t);
          setState((s) =>
            s && !s.done
              ? { ...s, done: true, error: s.error || `lost result: ${e instanceof Error ? e.message : e}` }
              : s,
          );
        });
    }, POLL_MS);
    return () => clearInterval(t);
  }, [state?.resultId, state?.done]);

  // Mount: reap buffers a previous page load may have orphaned under this
  // tabID (a reload never runs the unmount cleanup below). No session exists
  // yet at mount, so closing is free.
  // Tab closed: release the result buffer and the dedicated session.
  useEffect(() => {
    void QueryService.CloseTab(connID, tabID);
    return () => {
      const cur = stateRef.current;
      if (cur) void QueryService.CloseResult(cur.resultId);
      void QueryService.CloseTab(connID, tabID);
    };
  }, [connID, tabID]);

  const execute = useCallback(
    async (sql: string, maxRows: number, kind: PlanKind) => {
      if (!sql || (stateRef.current && !stateRef.current.done)) return;
      setError(null);
      setSessionReset(false);
      setCsvPath(null);
      setPlanKind(kind);
      setPlanText(null);
      try {
        const prev = stateRef.current?.resultId;
        // -1 ("Fetch All") is bounded by the preference ceiling; 0 keeps the
        // backend's default cap.
        const effectiveMax = maxRows < 0 ? Math.max(1, maxRowsPref) : maxRows;
        const res = await QueryService.Run(connID, tabID, sql, effectiveMax);
        if (prev) void QueryService.CloseResult(prev);
        setSort(null);
        setVersion((v) => v + 1);
        setState(res?.state ?? null);
        setSessionReset(res?.sessionReset ?? false);
      } catch (e) {
        setError(e instanceof Error ? e.message : String(e));
        setState(null);
        setPlanKind(null);
      }
    },
    [connID, tabID, maxRowsPref],
  );

  const run = useCallback(
    (maxRows: number) => execute(editor.current?.getStatementToRun() ?? "", maxRows, null),
    [execute],
  );

  // EXPLAIN runs go through the normal Run pipeline (same session, polling,
  // cancel, auto-timeout); only the rendering differs.
  const explain = useCallback(
    (analyze: boolean) => {
      const stmt = (editor.current?.getStatementToRun() ?? "").replace(STRIP_EXPLAIN, "");
      if (!stmt) return;
      const sql = analyze ? `EXPLAIN ANALYZE ${stmt}` : `EXPLAIN FORMAT=TREE ${stmt}`;
      void execute(sql, 0, analyze ? "analyze" : "explain");
    },
    [execute],
  );

  const onAnalyze = useCallback(() => {
    const stmt = (editor.current?.getStatementToRun() ?? "").replace(STRIP_EXPLAIN, "");
    const readOnly = /^\s*(select|with|table)\b/i.test(stmt);
    if (!readOnly && !armedAnalyze) {
      setArmedAnalyze(true);
      armTimer.current = window.setTimeout(() => setArmedAnalyze(false), 4000);
      return;
    }
    if (armTimer.current) window.clearTimeout(armTimer.current);
    setArmedAnalyze(false);
    explain(true);
  }, [armedAnalyze, explain]);

  // Editability is resolved once per finished result; a new run or an
  // explain clears staged edits with it.
  useEffect(() => {
    setEditInfo(null);
    setEdits(new Map());
    setApplyNote(null);
    if (!state?.done || state.error || planKind) return;
    let alive = true;
    QueryService.EditInfo(state.resultId)
      .then((i) => alive && setEditInfo(i))
      .catch(() => undefined); // not editable is the safe default
    return () => {
      alive = false;
    };
  }, [state?.done, state?.resultId, state?.error, planKind]);

  const editProps = useMemo(() => {
    const cols = state?.columns;
    if (!editInfo?.editable || !cols) return undefined;
    const colIdx = new Map(cols.map((c, i) => [c.name, i]));
    const editableCols = new Set(
      (editInfo.editableCols ?? [])
        .map((n) => colIdx.get(n ?? ""))
        .filter((i): i is number => i !== undefined),
    );
    const keyIdxs = (editInfo.keyCols ?? []).flatMap((n) => {
      const idx = colIdx.get(n ?? "");
      return idx === undefined ? [] : [{ name: n ?? "", idx }];
    });
    const staged = new Map([...edits].map(([k, e]) => [k, e.value]));
    return {
      editableCols,
      keyIdxs,
      staged,
      onStage: (e: StagedEdit) =>
        setEdits((prev) => new Map(prev).set(`${e.row}:${e.colIdx}`, e)),
    };
  }, [editInfo, state?.columns, edits]);

  const cellEdits = useCallback(
    () => [...edits.values()].map((e) => ({ key: e.key, col: e.col, value: e.value })),
    [edits],
  );

  const openPreview = useCallback(() => {
    const cur = stateRef.current;
    if (!cur) return;
    QueryService.PreviewEdits(cur.resultId, cellEdits())
      .then((stmts) => setPreview((stmts ?? []).filter((s): s is string => s !== null)))
      .catch((e) => setError(e instanceof Error ? e.message : String(e)));
  }, [cellEdits]);

  const applyEdits = useCallback(() => {
    const cur = stateRef.current;
    if (!cur) return;
    QueryService.ApplyEdits(cur.resultId, cellEdits())
      .then((n) => {
        setPreview(null);
        setEdits(new Map());
        setVersion((v) => v + 1); // refetch windows from the patched buffer
        setApplyNote(`${n} row${n === 1 ? "" : "s"} updated`);
      })
      .catch((e) => {
        setPreview(null);
        setError(e instanceof Error ? e.message : String(e));
      });
  }, [cellEdits]);

  const loadSnippets = useCallback(() => {
    QueryService.Snippets()
      .then((r) => setSnippets((r ?? []).filter((x): x is Snippet => x !== null)))
      .catch(() => setSnippets([]));
  }, []);

  const saveSnippet = useCallback(() => {
    if (!snippetName?.trim() || !snippetSQL.trim()) return;
    QueryService.SaveSnippet(snippetName, snippetSQL)
      .then(() => {
        setSnippetName(null);
        loadSnippets();
      })
      .catch((e) => setError(e instanceof Error ? e.message : String(e)));
  }, [snippetName, snippetSQL, loadSnippets]);

  // The plan is tiny (one TEXT cell); pull it whole once the run finishes.
  useEffect(() => {
    if (!planKind || !state?.done || state.error) return;
    let alive = true;
    QueryService.Rows(state.resultId, 0, Math.max(1, state.rowCount))
      .then((w) => {
        if (!alive || !w) return;
        const text = (w.rows ?? [])
          .map((r) => (r ?? []).filter((c): c is string => c !== null).join("\t"))
          .join("\n");
        setPlanText(text);
      })
      .catch((e) => {
        if (alive) setError(e instanceof Error ? e.message : String(e));
      });
    return () => {
      alive = false;
    };
  }, [planKind, state?.done, state?.resultId, state?.error, state?.rowCount]);

  const cancel = useCallback(() => {
    if (stateRef.current) void QueryService.Cancel(stateRef.current.resultId);
  }, []);

  const onSort = useCallback(
    async (col: number) => {
      const cur = stateRef.current;
      if (!cur || !cur.done) return;
      if (edits.size > 0) {
        setError("apply or discard the staged edits before sorting");
        return;
      }
      const desc = sort?.col === col ? !sort.desc : false;
      try {
        await QueryService.Sort(cur.resultId, col, desc);
        setSort({ col, desc });
        setVersion((v) => v + 1);
      } catch (e) {
        setError(e instanceof Error ? e.message : String(e));
      }
    },
    [sort, edits.size],
  );

  return (
    <div className="flex h-full flex-col">
      <div className="flex flex-col gap-2 p-3 pb-2">
        {schema !== null && (
          <div style={{ height: editorH }}>
            <SqlEditor
              ref={editor}
              initial={initialSQL}
              schema={schema}
              onRun={() => void run(0)}
              onChange={(sqlText) => setTabSQL(tabID, sqlText)}
              onFormatted={setError}
            />
          </div>
        )}
        <div className="flex items-center gap-3">
          <Button size="sm" onClick={() => void run(0)} disabled={running}>
            Run
          </Button>
          <Button size="sm" variant="outline" onClick={() => explain(false)} disabled={running}>
            Explain
          </Button>
          <Button
            size="sm"
            variant={armedAnalyze ? "destructive" : "outline"}
            title="Runs EXPLAIN ANALYZE — the statement is actually executed"
            onClick={onAnalyze}
            disabled={running}
          >
            {armedAnalyze ? "Executes Statement — Confirm" : "Explain Analyze"}
          </Button>
          <Button
            size="sm"
            variant="outline"
            title="Format the selection or the whole buffer (Ctrl+Shift+F)"
            onClick={() => editor.current?.format()}
          >
            Format
          </Button>
          <DropdownMenu onOpenChange={(open) => open && loadSnippets()}>
            <DropdownMenuTrigger className="inline-flex h-8 items-center rounded-lg border border-input px-3 text-sm hover:bg-muted/50">
              Snippets
            </DropdownMenuTrigger>
            <DropdownMenuContent align="start" className="max-w-md text-xs">
              <DropdownMenuItem
                onClick={() => {
                  setSnippetSQL(editor.current?.getStatementToRun() ?? "");
                  setSnippetName("");
                }}
              >
                Save Current Statement…
              </DropdownMenuItem>
              {snippets.length > 0 && <DropdownMenuSeparator />}
              {snippets.map((sn) => (
                <DropdownMenuItem
                  key={sn.id}
                  title={sn.sql}
                  onClick={() => editor.current?.insert(sn.sql)}
                >
                  <span className="min-w-0 flex-1 truncate">{sn.name}</span>
                  <button
                    type="button"
                    className="ml-2 rounded p-0.5 text-muted-foreground hover:text-destructive"
                    title="Delete snippet"
                    onClick={(e) => {
                      e.stopPropagation();
                      void QueryService.DeleteSnippet(sn.id).then(loadSnippets);
                    }}
                  >
                    <X className="h-3 w-3" />
                  </button>
                </DropdownMenuItem>
              ))}
            </DropdownMenuContent>
          </DropdownMenu>
          {running && (
            <Button size="sm" variant="destructive" onClick={cancel}>
              Cancel
            </Button>
          )}
          <span className="text-xs text-muted-foreground">
            Ctrl+Enter runs the statement at the cursor (or selection)
          </span>
          {state && (
            <span className="text-xs tabular-nums text-muted-foreground">
              {state.rowCount.toLocaleString()} rows{running ? "…" : ""}
            </span>
          )}
          {state?.capped && (
            <span className="flex items-center gap-2 text-xs text-warning">
              capped at {state.rowCount.toLocaleString()}
              <Button size="xs" variant="outline" onClick={() => void run(-1)}>
                Fetch All
              </Button>
            </span>
          )}
          {sessionReset && (
            <span className="text-xs text-warning">
              session was reset — SET/USE/transaction state is gone
            </span>
          )}
          {state?.done && state.rowCount > 0 && !planKind && (
            <Button
              size="xs"
              variant="outline"
              onClick={() =>
                void QueryService.ExportCSV(state.resultId)
                  .then(setCsvPath)
                  .catch((e) => setError(e instanceof Error ? e.message : String(e)))
              }
            >
              Export CSV
            </Button>
          )}
          {csvPath && (
            <span className="font-mono text-xs text-muted-foreground">→ {csvPath}</span>
          )}
          {edits.size > 0 && (
            <>
              <Button size="xs" onClick={openPreview}>
                Apply {edits.size} Edit{edits.size === 1 ? "" : "s"}
              </Button>
              <Button size="xs" variant="ghost" onClick={() => setEdits(new Map())}>
                Discard
              </Button>
            </>
          )}
          {applyNote && edits.size === 0 && (
            <span className="text-xs text-success">{applyNote}</span>
          )}
          {editInfo?.editable && edits.size === 0 && !applyNote && (
            <span className="text-xs text-muted-foreground">
              double-click a cell to edit
            </span>
          )}
        </div>
        {(error || state?.error) && (
          <pre className="whitespace-pre-wrap rounded-md border border-destructive/50 bg-destructive/10 p-2 font-mono text-xs text-destructive">
            {error ?? state?.error}
          </pre>
        )}
      </div>

      <div
        className="h-1 shrink-0 cursor-row-resize border-t hover:bg-primary/40"
        title="Drag to resize the editor"
        onMouseDown={(e) => {
          const start = editorH;
          startDrag(e, "row-resize", (_dx, dy) =>
            setTabEditorH(
              tabID,
              Math.min(Math.round(window.innerHeight * 0.75), Math.max(80, start + dy)),
            ),
          );
        }}
      />

      <div className="min-h-0 flex-1">
        {state && planKind && planText !== null && <PlanView text={planText} />}
        {state && !planKind && (
          <Grid
            resultId={state.resultId}
            columns={state.columns ?? []}
            rowCount={state.rowCount}
            version={version}
            sort={sort}
            sortable={state.done}
            onSort={(c) => void onSort(c)}
            edit={editProps}
            insertTarget={
              editInfo?.editable && editInfo.schema && editInfo.table
                ? { schema: editInfo.schema, table: editInfo.table }
                : undefined
            }
          />
        )}
      </div>

      <Dialog open={snippetName !== null} onOpenChange={(o) => !o && setSnippetName(null)}>
        <DialogContent className="sm:max-w-lg">
          <DialogHeader>
            <DialogTitle>Save Snippet</DialogTitle>
          </DialogHeader>
          <input
            autoFocus
            placeholder="Snippet Name"
            className="h-8 w-full rounded-md border bg-transparent px-2 text-sm outline-none focus:border-ring"
            value={snippetName ?? ""}
            onChange={(e) => setSnippetName(e.target.value)}
            onKeyDown={(e) => e.key === "Enter" && saveSnippet()}
          />
          <pre className="max-h-48 overflow-auto whitespace-pre-wrap break-all rounded bg-muted/40 p-2 font-mono text-xs">
            {snippetSQL || "(empty — put the cursor on a statement first)"}
          </pre>
          <div className="flex justify-end gap-2">
            <Button size="sm" variant="outline" onClick={() => setSnippetName(null)}>
              Cancel
            </Button>
            <Button size="sm" onClick={saveSnippet} disabled={!snippetName?.trim() || !snippetSQL.trim()}>
              Save
            </Button>
          </div>
        </DialogContent>
      </Dialog>

      <Dialog open={preview !== null} onOpenChange={(o) => !o && setPreview(null)}>
        <DialogContent className="sm:max-w-2xl">
          <DialogHeader>
            <DialogTitle>Apply Edits</DialogTitle>
          </DialogHeader>
          <p className="text-xs text-muted-foreground">
            These statements run in one transaction on this tab&apos;s session:
          </p>
          <pre className="max-h-80 overflow-auto whitespace-pre-wrap break-all rounded bg-muted/40 p-3 font-mono text-xs">
            {(preview ?? []).join("\n")}
          </pre>
          <div className="flex justify-end gap-2">
            <Button size="sm" variant="outline" onClick={() => setPreview(null)}>
              Cancel
            </Button>
            <Button size="sm" onClick={applyEdits}>
              Run Updates
            </Button>
          </div>
        </DialogContent>
      </Dialog>
    </div>
  );
}
