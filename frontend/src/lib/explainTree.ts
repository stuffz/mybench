// Parser for MySQL EXPLAIN FORMAT=TREE / EXPLAIN ANALYZE output (8.0.16+).
// Both emit the same indented text; ANALYZE adds "(actual ...)" groups. The
// parser is defensive: anything it cannot shape into a tree is rendered raw
// by PlanView, so an unexpected server format degrades, never breaks.

export type PlanNode = {
  name: string;
  estCost?: number;
  estRows?: number;
  actualFirstMs?: number;
  actualLastMs?: number; // time to last row, per loop
  actualRows?: number; // avg rows per loop
  loops?: number;
  neverExecuted?: boolean;
  // est vs actual rows off by ≥100× — the classic "optimizer had no idea"
  misestimate?: boolean;
  children: PlanNode[];
};

// MySQL prints sub-ms times in scientific notation ("599e-6"), so numbers
// need the exponent form, negative exponents included.
const NUM = String.raw`[\d.]+(?:[eE][-+]?\d+)?`;
const NODE_RE = /^(\s*)-> (.*)$/;
const EST_RE = new RegExp(String.raw`\s*\(cost=(${NUM})(?:\.\.${NUM})?\s+rows=(${NUM})\)`);
const ACTUAL_RE = new RegExp(
  String.raw`\s*\(actual time=(${NUM})\.\.(${NUM})\s+rows=(${NUM})\s+loops=(${NUM})\)`,
);
const NEVER_RE = /\s*\(never executed\)/;

function parseNode(rest: string): PlanNode {
  const node: PlanNode = { name: rest.trim(), children: [] };
  let name = rest;

  const est = EST_RE.exec(name);
  if (est) {
    node.estCost = Number(est[1]);
    node.estRows = Number(est[2]);
    name = name.replace(EST_RE, "");
  }
  const actual = ACTUAL_RE.exec(name);
  if (actual) {
    node.actualFirstMs = Number(actual[1]);
    node.actualLastMs = Number(actual[2]);
    node.actualRows = Number(actual[3]);
    node.loops = Number(actual[4]);
    name = name.replace(ACTUAL_RE, "");
  }
  if (NEVER_RE.test(name)) {
    node.neverExecuted = true;
    name = name.replace(NEVER_RE, "");
  }
  node.name = name.trim();

  if (node.estRows !== undefined && node.actualRows !== undefined && node.estRows > 0) {
    const ratio = Math.max(node.actualRows / node.estRows, node.estRows / node.actualRows);
    node.misestimate = ratio >= 100;
  }
  return node;
}

// Returns the root nodes (normally one), or [] when the text has no
// "-> " lines — the caller falls back to raw display.
export function parsePlanTree(text: string): PlanNode[] {
  const roots: PlanNode[] = [];
  // (depth, node) stack; a new node attaches to the nearest shallower entry.
  const stack: { depth: number; node: PlanNode }[] = [];

  for (const line of text.split("\n")) {
    const m = NODE_RE.exec(line);
    if (!m) {
      // Continuation text (not seen in practice; kept so nothing is dropped).
      const top = stack[stack.length - 1];
      if (top && line.trim()) top.node.name += " " + line.trim();
      continue;
    }
    const depth = m[1].length;
    const node = parseNode(m[2]);
    while (stack.length && stack[stack.length - 1].depth >= depth) stack.pop();
    if (stack.length === 0) roots.push(node);
    else stack[stack.length - 1].node.children.push(node);
    stack.push({ depth, node });
  }
  return roots;
}

// Compact row/cost formatting: 1234567 → "1.23M".
export function fmtNum(n: number): string {
  if (!Number.isFinite(n)) return String(n);
  if (n >= 1e9) return (n / 1e9).toPrecision(3) + "G";
  if (n >= 1e6) return (n / 1e6).toPrecision(3) + "M";
  if (n >= 1e3) return (n / 1e3).toPrecision(3) + "k";
  if (Number.isInteger(n)) return String(n);
  return n.toPrecision(3);
}

// Milliseconds with sane units: 0.034 → "34µs", 1234 → "1.23s".
export function fmtMs(ms: number): string {
  if (ms >= 1000) return (ms / 1000).toPrecision(3) + "s";
  if (ms >= 1) return ms.toPrecision(3) + "ms";
  return (ms * 1000).toPrecision(3) + "µs";
}
