import { memo, useCallback, useEffect, useMemo, useRef, useState, type MouseEvent as ReactMouseEvent } from "react";
import { useVirtualizer } from "@tanstack/react-virtual";
import type { Column } from "@/lib/api";
import { useResultWindow } from "@/hooks/useResultWindow";
import { ContextMenu, type CtxMenuItem } from "@/components/ContextMenu";
import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";

// Virtualized grid: plain divs, no per-cell components in the hot path
// (SPEC.md perf discipline). Rows are memoized so a scroll frame re-renders
// only rows entering the viewport — WebKitGTK chokes on reconciling every
// visible row per frame, which Chromium hid during browser-mode dev. Widths
// are pixels computed once; per-row CSS containment keeps layout local.

const ROW_H = 28;
const CH_PX = 8; // ≈1ch of 13px RobotoMono; computed once, avoids ch-unit layout

// Auto-fit bounds: sample the first window only, clamp each column between
// FIT_MIN_CH and FIT_MAX_CH characters.
const FIT_SAMPLE = 200;
const FIT_MIN_CH = 12;
const FIT_MAX_CH = 60;

export type SortState = { col: number; desc: boolean } | null;

type CellRow = (string | null)[];

// One staged (not yet applied) cell change, identity captured from the key
// columns' current values so the backend can build the WHERE clause.
export type StagedEdit = {
  row: number;
  colIdx: number;
  col: string;
  key: Record<string, string | null>;
  value: string | null;
};

export type EditProps = {
  editableCols: Set<number>;
  keyIdxs: { name: string; idx: number }[];
  // `${row}:${colIdx}` → staged value, overlays the buffered cell.
  staged: Map<string, string | null>;
  onStage: (e: StagedEdit) => void;
};

type RowProps = {
  row: CellRow | null;
  index: number;
  widths: number[];
  start: number;
  onCell: (col: number, value: string | null) => void;
  staged?: Map<string, string | null>;
  editableCols?: Set<number>;
  onCellDbl?: (row: number, col: number) => void;
  onCellCtx: (e: ReactMouseEvent, row: number, col: number) => void;
};

// memo: props are reference-stable per index across scroll frames (row comes
// from the window cache, widths/onCell are memoized, start is fixed per
// index), so scrolling skips re-rendering rows already on screen. Staged
// edits change the map identity — one re-render of visible rows per stage.
const GridRow = memo(function GridRow({
  row,
  index,
  widths,
  start,
  onCell,
  staged,
  editableCols,
  onCellDbl,
  onCellCtx,
}: RowProps) {
  return (
    <div
      className="absolute left-0 flex w-full border-b border-border/40 hover:bg-muted/30"
      style={{ transform: `translateY(${start}px)`, height: ROW_H, contain: "layout paint" }}
    >
      {row ? (
        row.map((cell, j) => {
          const stagedVal = staged?.get(`${index}:${j}`);
          const isStaged = stagedVal !== undefined;
          const shown = isStaged ? stagedVal : cell;
          return (
            <div
              key={j}
              className={
                "shrink-0 cursor-pointer truncate px-2 leading-7 " +
                (shown === null ? "italic text-muted-foreground " : "") +
                (isStaged ? "bg-warning/15 text-warning" : "")
              }
              style={{ width: widths[j] }}
              title={shown ?? undefined}
              onClick={() => onCell(j, shown)}
              onDoubleClick={editableCols?.has(j) ? () => onCellDbl?.(index, j) : undefined}
              onContextMenu={(e) => onCellCtx(e, index, j)}
            >
              {shown === null ? "NULL" : shown}
            </div>
          );
        })
      ) : (
        <div className="px-2 leading-7 text-muted-foreground">…</div>
      )}
    </div>
  );
});

type Props = {
  resultId: string;
  columns: Column[];
  rowCount: number;
  version: number;
  sort: SortState;
  sortable: boolean;
  onSort: (col: number) => void;
  edit?: EditProps;
  // Known single-table origin — enables "Copy Row as INSERT".
  insertTarget?: { schema: string; table: string };
};

// SQL literal for one cell in a generated INSERT (values are strings on the
// wire; the server coerces). Mirrors internal/sqlesc.Value — keep in sync.
function sqlLit(v: string | null): string {
  if (v === null) return "NULL";
  return (
    "'" +
    v
      .replace(/\\/g, "\\\\")
      .replace(/'/g, "''")
      .replace(/\u0000/g, "\\0")
      .replace(/\n/g, "\\n")
      .replace(/\r/g, "\\r")
      .replace(/\u001a/g, "\\Z") +
    "'"
  );
}

function quoteId(id: string): string {
  return "`" + id.replace(/`/g, "``") + "`";
}

export function Grid({
  resultId,
  columns,
  rowCount,
  version,
  sort,
  sortable,
  onSort,
  edit,
  insertTarget,
}: Props) {
  const parentRef = useRef<HTMLDivElement>(null);
  const getRow = useResultWindow(resultId, rowCount, version);
  // Cell inspector: full value in a dialog (cells themselves truncate).
  const [inspect, setInspect] = useState<{ col: string; value: string | null } | null>(null);
  // In-place cell editor (double-click); Enter stages, Escape cancels.
  const [editing, setEditing] = useState<{
    row: number;
    col: number;
    value: string;
  } | null>(null);
  // With editing enabled, a single click waits out the double-click window
  // before opening the inspector; without it, the inspector stays instant.
  const clickTimer = useRef<number | null>(null);
  // Right-click copy menu on a cell.
  const [menu, setMenu] = useState<{ x: number; y: number; row: number; col: number } | null>(
    null,
  );

  // Keyed by names, not array identity — state polls during streaming rebuild
  // the columns array every 250ms and must not reset widths each time.
  const colKey = useMemo(() => columns.map((c) => c.name).join("\u0000"), [columns]);
  const defaultWidths = useMemo(
    () => columns.map((c) => Math.min(Math.max(c.name.length + 4, 12), 44) * CH_PX),
    [colKey], // eslint-disable-line react-hooks/exhaustive-deps
  );
  // Columns are drag-resizable; new result shape resets to the heuristic.
  const [widths, setWidths] = useState(defaultWidths);
  useEffect(() => setWidths(defaultWidths), [defaultWidths]);

  // Auto-fit once per result: follow the widest cell in the sampled first
  // window, capped so one huge value can't blow up the layout (the cell
  // inspector shows full values). Runs on every render until the window has
  // landed; afterwards manual drag-resizes and sorts leave widths alone.
  const fittedFor = useRef<string | null>(null);
  useEffect(() => {
    if (fittedFor.current === resultId || rowCount === 0) return;
    const sample: CellRow[] = [];
    for (let i = 0; i < Math.min(rowCount, FIT_SAMPLE); i++) {
      const r = getRow(i);
      if (r) sample.push(r);
    }
    if (sample.length === 0) return;
    fittedFor.current = resultId;
    setWidths(
      columns.map((c, j) => {
        let content = 4; // "NULL"
        for (const r of sample) {
          const len = r[j]?.length ?? 4;
          if (len > content) content = len;
        }
        return Math.min(Math.max(c.name.length + 4, content + 2, FIT_MIN_CH), FIT_MAX_CH) * CH_PX;
      }),
    );
  });
  const totalPx = useMemo(() => widths.reduce((a, b) => a + b, 0), [widths]);

  const startResize = useCallback((i: number, e: ReactMouseEvent) => {
    e.preventDefault();
    e.stopPropagation();
    const startX = e.clientX;
    let startW = 0;
    setWidths((w) => {
      startW = w[i];
      return w;
    });
    document.body.style.cursor = "col-resize";
    document.body.style.userSelect = "none";
    const move = (ev: MouseEvent) =>
      setWidths((w) => {
        const next = [...w];
        next[i] = Math.max(48, startW + ev.clientX - startX);
        return next;
      });
    const up = () => {
      document.body.style.cursor = "";
      document.body.style.userSelect = "";
      window.removeEventListener("mousemove", move);
      window.removeEventListener("mouseup", up);
    };
    window.addEventListener("mousemove", move);
    window.addEventListener("mouseup", up);
  }, []);

  const editRef = useRef(edit);
  editRef.current = edit;

  const onCell = useCallback(
    (col: number, value: string | null) => {
      const open = () => setInspect({ col: columns[col]?.name ?? "", value });
      if (!editRef.current) {
        open();
        return;
      }
      if (clickTimer.current) window.clearTimeout(clickTimer.current);
      clickTimer.current = window.setTimeout(open, 300);
    },
    [columns],
  );

  const startEdit = useCallback(
    (row: number, col: number) => {
      const e = editRef.current;
      if (!e) return;
      if (clickTimer.current) window.clearTimeout(clickTimer.current);
      const r = getRow(row);
      if (!r) return;
      const stagedVal = e.staged.get(`${row}:${col}`);
      const current = stagedVal !== undefined ? stagedVal : r[col];
      setEditing({ row, col, value: current ?? "" });
    },
    [getRow],
  );

  const commitEdit = useCallback(
    (asNull: boolean) => {
      const e = editRef.current;
      setEditing((cur) => {
        if (!cur || !e) return null;
        const r = getRow(cur.row);
        if (!r) return null;
        const key: Record<string, string | null> = {};
        for (const k of e.keyIdxs) key[k.name] = r[k.idx] ?? null;
        e.onStage({
          row: cur.row,
          colIdx: cur.col,
          col: columns[cur.col]?.name ?? "",
          key,
          value: asNull ? null : cur.value,
        });
        return null;
      });
    },
    [getRow, columns],
  );

  const onCellCtx = useCallback((e: ReactMouseEvent, row: number, col: number) => {
    e.preventDefault();
    if (clickTimer.current) window.clearTimeout(clickTimer.current);
    setMenu({ x: e.clientX, y: e.clientY, row, col });
  }, []);

  const menuItems = (m: { row: number; col: number }): CtxMenuItem[] => {
    const row = getRow(m.row);
    if (!row) return [];
    const cell = row[m.col] ?? null;
    const copy = (text: string) => void navigator.clipboard.writeText(text);
    const items: CtxMenuItem[] = [
      { label: "Copy Cell", onClick: () => copy(cell ?? "NULL") },
      {
        label: "Copy Row (Tab-Separated)",
        onClick: () => copy(row.map((c) => c ?? "NULL").join("\t")),
      },
      {
        label: "Copy Row as JSON",
        onClick: () =>
          copy(
            JSON.stringify(
              Object.fromEntries(columns.map((c, i) => [c.name, row[i] ?? null])),
              null,
              2,
            ),
          ),
      },
    ];
    if (insertTarget) {
      items.push({
        label: "Copy Row as INSERT",
        onClick: () =>
          copy(
            `INSERT INTO ${quoteId(insertTarget.schema)}.${quoteId(insertTarget.table)} ` +
              `(${columns.map((c) => quoteId(c.name)).join(", ")}) VALUES ` +
              `(${row.map((c) => sqlLit(c ?? null)).join(", ")});`,
          ),
      });
    }
    return items;
  };

  const virt = useVirtualizer({
    count: rowCount,
    getScrollElement: () => parentRef.current,
    estimateSize: () => ROW_H,
    overscan: 12,
  });

  return (
    <div ref={parentRef} className="h-full overflow-auto font-mono text-xs">
      <div style={{ width: totalPx, minWidth: "100%" }}>
        <div className="sticky top-0 z-10 flex border-b bg-background">
          {columns.map((c, i) => (
            <div
              key={c.name + i}
              className="relative shrink-0 select-none"
              style={{ width: widths[i] }}
            >
              <button
                type="button"
                onClick={() => sortable && onSort(i)}
                className="w-full cursor-pointer truncate px-2 py-1 text-left hover:bg-muted/50"
                title={`${c.name} (${c.type})`}
              >
                <span className="font-medium text-foreground">
                  {c.name}
                  {sort?.col === i ? (sort.desc ? " ↓" : " ↑") : ""}
                </span>
                <span className="block truncate text-[10px] font-normal text-muted-foreground">
                  {c.type}
                </span>
              </button>
              <div
                className="absolute right-0 top-0 h-full w-1.5 cursor-col-resize hover:bg-primary/50"
                onMouseDown={(e) => startResize(i, e)}
              />
            </div>
          ))}
        </div>
        <div
          style={{ height: virt.getTotalSize(), position: "relative" }}
          className="w-full"
        >
          {virt.getVirtualItems().map((vi) => (
            <GridRow
              key={vi.key}
              row={getRow(vi.index)}
              index={vi.index}
              widths={widths}
              start={vi.start}
              onCell={onCell}
              staged={edit?.staged}
              editableCols={edit?.editableCols}
              onCellDbl={startEdit}
              onCellCtx={onCellCtx}
            />
          ))}
          {editing && (
            <div
              className="absolute z-20 flex items-center gap-1"
              style={{
                top: editing.row * ROW_H,
                left: widths.slice(0, editing.col).reduce((a, b) => a + b, 0),
                height: ROW_H,
              }}
            >
              <input
                autoFocus
                className="h-full border border-primary bg-background px-2 font-mono text-xs outline-none"
                style={{ width: Math.max(widths[editing.col], 120) }}
                value={editing.value}
                onChange={(ev) =>
                  setEditing((cur) => (cur ? { ...cur, value: ev.target.value } : cur))
                }
                onKeyDown={(ev) => {
                  if (ev.key === "Enter") commitEdit(false);
                  else if (ev.key === "Escape") setEditing(null);
                }}
                onBlur={() => setEditing(null)}
              />
              <button
                type="button"
                title="Stage NULL"
                className="h-full rounded border bg-background px-2 text-muted-foreground hover:text-foreground"
                onMouseDown={(ev) => {
                  ev.preventDefault(); // fires before the input blur cancels
                  commitEdit(true);
                }}
              >
                ∅
              </button>
            </div>
          )}
        </div>
      </div>

      {menu && (
        <ContextMenu x={menu.x} y={menu.y} items={menuItems(menu)} onClose={() => setMenu(null)} />
      )}

      <Dialog open={inspect !== null} onOpenChange={(o) => !o && setInspect(null)}>
        <DialogContent className="max-w-2xl">
          <DialogHeader>
            <DialogTitle className="font-mono text-sm">{inspect?.col}</DialogTitle>
          </DialogHeader>
          <pre className="max-h-96 overflow-auto whitespace-pre-wrap break-all rounded bg-muted/40 p-3 font-mono text-xs">
            {inspect?.value === null ? "NULL" : inspect?.value}
          </pre>
        </DialogContent>
      </Dialog>
    </div>
  );
}
