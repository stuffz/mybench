import { useMemo, useState } from "react";
import { Button } from "@/components/ui/button";
import { fmtMs, fmtNum, parsePlanTree, type PlanNode } from "@/lib/explainTree";

// Renders EXPLAIN FORMAT=TREE / EXPLAIN ANALYZE output as a collapsible tree
// (SPEC.md: tree view, not graphics). Falls back to the raw text when the
// server's output isn't TREE-shaped (older MySQL, MariaDB).

const INDENT_PX = 18;

function NodeRow({ node, depth }: { node: PlanNode; depth: number }) {
  const [open, setOpen] = useState(true);
  const kids = node.children.length > 0;
  return (
    <>
      <div
        className="flex items-baseline gap-2 border-b border-border/30 py-0.5 pr-2 hover:bg-muted/30"
        style={{ paddingLeft: depth * INDENT_PX + 6 }}
      >
        <button
          className={
            "w-4 shrink-0 select-none text-xs text-muted-foreground " +
            (kids ? "cursor-pointer hover:text-foreground" : "invisible")
          }
          onClick={() => setOpen((o) => !o)}
          title={open ? "Collapse" : "Expand"}
        >
          {open ? "▾" : "▸"}
        </button>
        <span className="min-w-0 truncate font-mono text-xs" title={node.name}>
          {node.name}
        </span>
        <span className="ml-auto flex shrink-0 items-baseline gap-3 font-mono text-[11px] tabular-nums">
          {node.estRows !== undefined && (
            <span
              className={node.misestimate ? "text-warning" : "text-muted-foreground"}
              title={
                "Optimizer estimate" +
                (node.estCost !== undefined ? ` — cost ${fmtNum(node.estCost)}` : "") +
                (node.misestimate ? " — off from actual by ≥100×" : "")
              }
            >
              ~{fmtNum(node.estRows)} rows
              {node.estCost !== undefined && (
                <span className="opacity-60"> · cost {fmtNum(node.estCost)}</span>
              )}
            </span>
          )}
          {node.neverExecuted && <span className="text-muted-foreground">never executed</span>}
          {node.actualLastMs !== undefined && (
            <span
              className={node.misestimate ? "text-warning" : "text-success/90"}
              title={`Actual — first row ${fmtMs(node.actualFirstMs ?? 0)}, all rows ${fmtMs(node.actualLastMs)}, per loop`}
            >
              {fmtMs(node.actualLastMs)} · {fmtNum(node.actualRows ?? 0)} rows
              {(node.loops ?? 1) > 1 && <span className="opacity-60"> ×{node.loops}</span>}
            </span>
          )}
        </span>
      </div>
      {open && node.children.map((c, i) => <NodeRow key={i} node={c} depth={depth + 1} />)}
    </>
  );
}

type Props = { text: string };

export function PlanView({ text }: Props) {
  const roots = useMemo(() => parsePlanTree(text), [text]);
  const [copied, setCopied] = useState(false);

  return (
    <div className="flex h-full flex-col overflow-auto">
      <div className="flex items-center gap-2 px-3 py-1.5">
        <span className="text-xs text-muted-foreground">Execution Plan</span>
        <Button
          size="xs"
          variant="outline"
          onClick={() => {
            void navigator.clipboard.writeText(text).then(() => {
              setCopied(true);
              setTimeout(() => setCopied(false), 1500);
            });
          }}
        >
          {copied ? "Copied" : "Copy Raw"}
        </Button>
      </div>
      {roots.length > 0 ? (
        <div className="min-w-fit pb-3">
          {roots.map((r, i) => (
            <NodeRow key={i} node={r} depth={0} />
          ))}
        </div>
      ) : (
        // Not TREE-shaped (older server / MariaDB / FORMAT override) — show as-is.
        <pre className="whitespace-pre px-3 pb-3 font-mono text-xs">{text}</pre>
      )}
    </div>
  );
}
