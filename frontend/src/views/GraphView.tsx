import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { AdminService } from "@/lib/api";
import type { Graph, GraphNode, GraphEdge } from "@/lib/api";
import { Button } from "@/components/ui/button";
import { Switch } from "@/components/ui/switch";
import { useApp } from "@/store";

// Whole-server FK graph (Obsidian-style): every user table is a node, every
// foreign key an edge; system schemas are excluded backend-side. Hand-rolled
// force layout + canvas — a few hundred nodes need no graph library.

type Props = { connID: string; active: boolean };

type SimNode = {
  id: string;
  schema: string;
  table: string;
  x: number;
  y: number;
  vx: number;
  vy: number;
  r: number;
  hue: number;
  deg: number;
  // schema-cluster anchor this node gravitates toward
  ax: number;
  ay: number;
};

type SimLink = { a: number; b: number };

function schemaHue(s: string): number {
  let h = 0;
  for (const ch of s) h = (h * 31 + ch.charCodeAt(0)) % 360;
  return h;
}

// fzf-style subsequence match: every query char must appear in order.
// Greedy left-to-right with a score favoring word starts (after `.`/`_`)
// and consecutive runs, penalizing gaps. Returns null on no match.
function fuzzyMatch(query: string, target: string): { score: number; pos: number[] } | null {
  const q = query.toLowerCase();
  const t = target.toLowerCase();
  const pos: number[] = [];
  let score = 0;
  let from = 0;
  for (const ch of q) {
    const idx = t.indexOf(ch, from);
    if (idx === -1) return null;
    const boundary = idx === 0 || t[idx - 1] === "." || t[idx - 1] === "_";
    const consecutive = pos.length > 0 && idx === pos[pos.length - 1] + 1;
    score += (boundary ? 3 : 0) + (consecutive ? 2 : 0) - (idx - from);
    pos.push(idx);
    from = idx + 1;
  }
  return { score, pos };
}

export function GraphView({ connID, active }: Props) {
  const wrap = useRef<HTMLDivElement>(null);
  const canvas = useRef<HTMLCanvasElement>(null);
  // Render-loop gates (refs so the long-lived rAF closure sees changes).
  const activeRef = useRef(active);
  activeRef.current = active;
  const needsDrawRef = useRef(true);
  useEffect(() => {
    if (active) needsDrawRef.current = true; // repaint on tab return
  }, [active]);
  const controls = useRef<{ fit?: () => void }>({});
  const [graph, setGraph] = useState<Graph | null>(null);
  const [hideIsolated, setHideIsolated] = useState(false);
  // Slider value persists via preferences ("everything saved constantly").
  const maxNode = useApp((s) => s.prefs.graphNodeSize);
  const setPrefs = useApp((s) => s.setPrefs);
  const setMaxNode = (v: number) => setPrefs({ graphNodeSize: v });
  const [error, setError] = useState<string | null>(null);
  const [counts, setCounts] = useState({ nodes: 0, edges: 0 });
  // Focus mode: show only tables within `hops` FK jumps of one table —
  // the way to find anything in a many-hundred-table server.
  const [focus, setFocus] = useState<string | null>(null);
  const [hops, setHops] = useState(2);
  const [findVal, setFindVal] = useState("");
  const [findOpen, setFindOpen] = useState(false);
  const [findSel, setFindSel] = useState(0);

  // "Show in Graph" from the sidebar context menu lands here via the store
  // (the graph tab is a singleton that may already be open).
  const focusReq = useApp((s) => s.graphFocus[connID]);
  useEffect(() => {
    if (focusReq) setFocus(`${focusReq.schema}.${focusReq.table}`);
  }, [focusReq]);

  const refresh = useCallback(async () => {
    try {
      setGraph(await AdminService.FKGraph(connID));
      setError(null);
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }, [connID]);

  const connected = useApp((s) => s.openIDs.includes(connID));
  useEffect(() => {
    if (active && connected && !graph) void refresh();
  }, [active, connected, graph, refresh]);

  useEffect(() => {
    const cv = canvas.current;
    const box = wrap.current;
    if (!cv || !box || !graph) return;

    // --- build simulation data -------------------------------------------
    const allNodes = (graph.nodes ?? []).filter((n): n is GraphNode => n !== null);
    const allEdges = (graph.edges ?? []).filter((e): e is GraphEdge => e !== null);
    const degree = new Map<string, number>();
    for (const e of allEdges) {
      const a = `${e.fromSchema}.${e.fromTable}`;
      const b = `${e.toSchema}.${e.toTable}`;
      degree.set(a, (degree.get(a) ?? 0) + 1);
      degree.set(b, (degree.get(b) ?? 0) + 1);
    }
    // Focus mode: BFS over FK edges (undirected) up to `hops` jumps. While
    // focused, "hide isolated" is moot — the reach set is the filter.
    let visible: GraphNode[];
    if (focus && allNodes.some((n) => `${n.schema}.${n.table}` === focus)) {
      const adj = new Map<string, Set<string>>();
      const link = (a: string, b: string) => {
        (adj.get(a) ?? adj.set(a, new Set()).get(a)!).add(b);
      };
      for (const e of allEdges) {
        const a = `${e.fromSchema}.${e.fromTable}`;
        const b = `${e.toSchema}.${e.toTable}`;
        link(a, b);
        link(b, a);
      }
      const reach = new Set([focus]);
      let frontier = [focus];
      for (let d = 0; d < hops && frontier.length > 0; d++) {
        const next: string[] = [];
        for (const id of frontier) {
          for (const nb of adj.get(id) ?? []) {
            if (!reach.has(nb)) {
              reach.add(nb);
              next.push(nb);
            }
          }
        }
        frontier = next;
      }
      visible = allNodes.filter((n) => reach.has(`${n.schema}.${n.table}`));
    } else {
      visible = hideIsolated
        ? allNodes.filter((n) => (degree.get(`${n.schema}.${n.table}`) ?? 0) > 0)
        : allNodes;
    }

    // Radius scales relative to the largest visible table, so 10k-row and
    // 20M-row databases both spread across the whole size range; the slider
    // sets the cap.
    const maxLog = Math.max(
      1,
      ...visible.map((n) => Math.log10(n.rows + 1)),
    );

    // Cluster by schema: schemas sit on a ring sized to their table counts,
    // each with its own spiral, and gravity pulls nodes to their schema
    // anchor — grouped clusters instead of one interleaved hairball.
    const counts = new Map<string, number>();
    for (const n of visible) counts.set(n.schema, (counts.get(n.schema) ?? 0) + 1);
    const schemaList = [...counts.keys()];
    const clusterRad = (c: number) => 20 * Math.sqrt(c) + 30;
    const circum = schemaList.reduce((a, s) => a + 2 * clusterRad(counts.get(s)!) + 60, 0);
    const ringR = schemaList.length > 1 ? Math.max(220, circum / (2 * Math.PI)) : 0;
    const anchors = new Map<string, { x: number; y: number }>();
    let acc = 0;
    for (const s of schemaList) {
      const span = (2 * clusterRad(counts.get(s)!) + 60) / Math.max(circum, 1);
      const a = (acc + span / 2) * 2 * Math.PI;
      anchors.set(s, { x: Math.cos(a) * ringR, y: Math.sin(a) * ringR });
      acc += span;
    }

    const withinIdx = new Map<string, number>();
    const nodes: SimNode[] = visible.map((n) => {
      const id = `${n.schema}.${n.table}`;
      const deg = degree.get(id) ?? 0;
      const anchor = anchors.get(n.schema)!;
      // Deterministic spiral within the cluster — stable layout run-to-run.
      const j = withinIdx.get(n.schema) ?? 0;
      withinIdx.set(n.schema, j + 1);
      const angle = j * 2.399963; // golden angle
      const rad = 14 * Math.sqrt(j + 1);
      const size =
        4 +
        (maxNode - 4) * (Math.log10(n.rows + 1) / maxLog) +
        Math.min(deg, 8) * 0.5;
      return {
        id,
        schema: n.schema,
        table: n.table,
        x: anchor.x + Math.cos(angle) * rad,
        y: anchor.y + Math.sin(angle) * rad,
        vx: 0,
        vy: 0,
        r: Math.min(maxNode, Math.max(3.5, size)),
        hue: schemaHue(n.schema),
        deg,
        ax: anchor.x,
        ay: anchor.y,
      };
    });
    const idx = new Map(nodes.map((n, i) => [n.id, i]));
    const links: SimLink[] = [];
    for (const e of allEdges) {
      const a = idx.get(`${e.fromSchema}.${e.fromTable}`);
      const b = idx.get(`${e.toSchema}.${e.toTable}`);
      if (a !== undefined && b !== undefined && a !== b) links.push({ a, b });
    }
    const neighbors = new Map<number, Set<number>>();
    for (const l of links) {
      (neighbors.get(l.a) ?? neighbors.set(l.a, new Set()).get(l.a)!).add(l.b);
      (neighbors.get(l.b) ?? neighbors.set(l.b, new Set()).get(l.b)!).add(l.a);
    }
    setCounts({ nodes: nodes.length, edges: links.length });

    // --- viewport / interaction state ------------------------------------
    let k = 1;
    let tx = 0;
    let ty = 0;
    let alpha = 1;
    let hovered = -1;
    let dragNode = -1;
    let panning = false;
    let lastX = 0;
    let lastY = 0;
    let raf = 0;
    let disposed = false;
    // Auto fit-to-view keeps the whole graph on screen until the user pans,
    // zooms or drags; the Fit button re-enables it.
    let userMoved = false;
    controls.current.fit = () => {
      userMoved = false;
    };

    const dpr = window.devicePixelRatio || 1;
    // Canvas font strings can't contain CSS variables; resolve once.
    const labelFont = getComputedStyle(cv).fontFamily || "sans-serif";
    const resize = () => {
      const r = box.getBoundingClientRect();
      cv.width = Math.max(1, Math.round(r.width * dpr));
      cv.height = Math.max(1, Math.round(r.height * dpr));
      cv.style.width = `${r.width}px`;
      cv.style.height = `${r.height}px`;
      tx = tx || r.width / 2;
      ty = ty || r.height / 2;
    };
    resize();
    const ro = new ResizeObserver(resize);
    ro.observe(box);

    const toWorld = (sx: number, sy: number) => ({ x: (sx - tx) / k, y: (sy - ty) / k });
    const hit = (sx: number, sy: number) => {
      const w = toWorld(sx, sy);
      for (let i = nodes.length - 1; i >= 0; i--) {
        const n = nodes[i];
        const dx = n.x - w.x;
        const dy = n.y - w.y;
        if (dx * dx + dy * dy <= (n.r + 3 / k) * (n.r + 3 / k)) return i;
      }
      return -1;
    };

    // --- physics ----------------------------------------------------------
    const tick = () => {
      // Repulsion (capped O(n²) — fine at this scale).
      for (let i = 0; i < nodes.length; i++) {
        const a = nodes[i];
        for (let j = i + 1; j < nodes.length; j++) {
          const b = nodes[j];
          let dx = a.x - b.x;
          let dy = a.y - b.y;
          let d2 = dx * dx + dy * dy;
          if (d2 < 1) {
            dx = ((i * 7919) % 13) - 6;
            dy = ((j * 104729) % 13) - 6;
            d2 = dx * dx + dy * dy || 1;
          }
          if (d2 > 250000) continue;
          const f = (2200 / d2) * alpha;
          const d = Math.sqrt(d2);
          const fx = (dx / d) * f;
          const fy = (dy / d) * f;
          a.vx += fx;
          a.vy += fy;
          b.vx -= fx;
          b.vy -= fy;
        }
      }
      // FK springs — rest length scales with the node sizes.
      for (const l of links) {
        const a = nodes[l.a];
        const b = nodes[l.b];
        const dx = b.x - a.x;
        const dy = b.y - a.y;
        const d = Math.sqrt(dx * dx + dy * dy) || 1;
        const f = (d - (60 + a.r + b.r)) * 0.02 * alpha;
        const fx = (dx / d) * f;
        const fy = (dy / d) * f;
        a.vx += fx;
        a.vy += fy;
        b.vx -= fx;
        b.vy -= fy;
      }
      // Cluster gravity + integrate.
      for (let i = 0; i < nodes.length; i++) {
        const n = nodes[i];
        n.vx += (n.ax - n.x) * 0.0045 * alpha;
        n.vy += (n.ay - n.y) * 0.0045 * alpha;
        if (i === dragNode) {
          n.vx = 0;
          n.vy = 0;
          continue;
        }
        n.vx *= 0.85;
        n.vy *= 0.85;
        n.x += n.vx;
        n.y += n.vy;
      }
      alpha *= 0.995;
    };

    // --- render -----------------------------------------------------------
    // Edge/label colors come from the theme tokens so every app theme stays
    // in palette (node fills keep their data-derived hues). Read per frame —
    // the loop redraws every frame anyway and this keeps theme switches live.
    const themeColors = () => {
      const s = getComputedStyle(document.documentElement);
      return {
        info: s.getPropertyValue("--info").trim() || "#78aaff",
        fg: s.getPropertyValue("--foreground").trim() || "#ebebf5",
        muted: s.getPropertyValue("--muted-foreground").trim() || "#8c8c9b",
      };
    };
    const draw = () => {
      const ctx = cv.getContext("2d");
      if (!ctx) return;
      const tc = themeColors();
      if (!userMoved && nodes.length > 0) {
        let minX = Infinity;
        let maxX = -Infinity;
        let minY = Infinity;
        let maxY = -Infinity;
        for (const n of nodes) {
          if (n.x - n.r < minX) minX = n.x - n.r;
          if (n.x + n.r > maxX) maxX = n.x + n.r;
          if (n.y - n.r < minY) minY = n.y - n.r;
          if (n.y + n.r > maxY) maxY = n.y + n.r;
        }
        const w = cv.width / dpr;
        const h = cv.height / dpr;
        const bw = Math.max(1, maxX - minX);
        const bh = Math.max(1, maxY - minY);
        k = Math.min(1.4, Math.min(w / bw, h / bh) * 0.9);
        tx = w / 2 - ((minX + maxX) / 2) * k;
        ty = h / 2 - ((minY + maxY) / 2) * k;
      }
      ctx.setTransform(1, 0, 0, 1, 0, 0);
      ctx.clearRect(0, 0, cv.width, cv.height);
      ctx.setTransform(dpr * k, 0, 0, dpr * k, dpr * tx, dpr * ty);

      const hoverSet = hovered >= 0 ? (neighbors.get(hovered) ?? new Set<number>()) : null;
      const dimmed = hovered >= 0;

      ctx.lineWidth = 1 / k;
      for (const l of links) {
        const lit = hovered >= 0 && (l.a === hovered || l.b === hovered);
        ctx.globalAlpha = lit ? 0.9 : dimmed ? 0.08 : 0.28;
        ctx.strokeStyle = lit ? tc.info : tc.muted;
        ctx.beginPath();
        ctx.moveTo(nodes[l.a].x, nodes[l.a].y);
        ctx.lineTo(nodes[l.b].x, nodes[l.b].y);
        ctx.stroke();
      }
      ctx.globalAlpha = 1;

      for (let i = 0; i < nodes.length; i++) {
        const n = nodes[i];
        const hoverFocus = i === hovered || (hoverSet?.has(i) ?? false);
        const a = dimmed && !hoverFocus ? 0.15 : 0.92;
        ctx.fillStyle = `hsla(${n.hue}, 55%, ${i === hovered ? 68 : 56}%, ${a})`;
        ctx.beginPath();
        ctx.arc(n.x, n.y, n.r, 0, Math.PI * 2);
        ctx.fill();
        // Ring marks the focused table (focus mode).
        if (n.id === focus) {
          ctx.lineWidth = 2 / k;
          ctx.strokeStyle = tc.fg;
          ctx.stroke();
          ctx.lineWidth = 1 / k;
        }
      }

      const labelAll = k > 1.4 || nodes.length <= 60;
      ctx.font = `${11 / k}px ui-monospace, monospace`;
      ctx.textAlign = "center";
      for (let i = 0; i < nodes.length; i++) {
        const n = nodes[i];
        const focus = i === hovered || (hoverSet?.has(i) ?? false);
        if (!labelAll && !focus) continue;
        if (dimmed && !focus) continue;
        ctx.globalAlpha = focus ? 0.95 : 0.7;
        ctx.fillStyle = focus ? tc.fg : tc.muted;
        ctx.fillText(i === hovered ? n.id : n.table, n.x, n.y - n.r - 4 / k);
      }
      ctx.globalAlpha = 1;

      // Zoomed out, name the clusters instead of the (unreadable) tables.
      if (k < 1.1 && schemaList.length > 1 && !dimmed) {
        const sums = new Map<string, { x: number; y: number; c: number }>();
        for (const n of nodes) {
          const s = sums.get(n.schema);
          if (s) {
            s.x += n.x;
            s.y += n.y;
            s.c++;
          } else {
            sums.set(n.schema, { x: n.x, y: n.y, c: 1 });
          }
        }
        ctx.font = `${Math.min(13 / k, 44)}px ${labelFont}`;
        for (const [s, sum] of sums) {
          ctx.fillStyle = `hsla(${schemaHue(s)}, 45%, 72%, 0.85)`;
          ctx.fillText(s, sum.x / sum.c, sum.y / sum.c - clusterRad(sum.c) - 8 / k);
        }
      }
    };

    // Idle-CPU rule: nothing is simulated or drawn while the tab is hidden,
    // and once the simulation settles the loop only redraws after input
    // (wheel/pan/drag/hover) marks the frame dirty.
    const loop = () => {
      if (disposed) return;
      raf = requestAnimationFrame(loop);
      if (!activeRef.current) return;
      if (alpha > 0.005) {
        tick();
        needsDrawRef.current = true;
      }
      if (needsDrawRef.current) {
        needsDrawRef.current = false;
        draw();
      }
    };
    raf = requestAnimationFrame(loop);

    // --- events -----------------------------------------------------------
    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      needsDrawRef.current = true;
      userMoved = true;
      const rect = cv.getBoundingClientRect();
      const sx = e.clientX - rect.left;
      const sy = e.clientY - rect.top;
      const w = toWorld(sx, sy);
      k = Math.min(6, Math.max(0.08, k * Math.exp(-e.deltaY * 0.0012)));
      tx = sx - w.x * k;
      ty = sy - w.y * k;
    };
    const onDown = (e: MouseEvent) => {
      if (e.button !== 0) return;
      needsDrawRef.current = true;
      userMoved = true;
      const rect = cv.getBoundingClientRect();
      const sx = e.clientX - rect.left;
      const sy = e.clientY - rect.top;
      const h = hit(sx, sy);
      if (h >= 0) {
        dragNode = h;
        alpha = Math.max(alpha, 0.3);
      } else {
        panning = true;
      }
      lastX = e.clientX;
      lastY = e.clientY;
    };
    const onMove = (e: MouseEvent) => {
      needsDrawRef.current = true;
      const rect = cv.getBoundingClientRect();
      const sx = e.clientX - rect.left;
      const sy = e.clientY - rect.top;
      if (dragNode >= 0) {
        const w = toWorld(sx, sy);
        nodes[dragNode].x = w.x;
        nodes[dragNode].y = w.y;
        alpha = Math.max(alpha, 0.3);
      } else if (panning) {
        tx += e.clientX - lastX;
        ty += e.clientY - lastY;
        lastX = e.clientX;
        lastY = e.clientY;
      } else {
        hovered = hit(sx, sy);
        cv.style.cursor = hovered >= 0 ? "pointer" : "grab";
      }
    };
    const onUp = () => {
      needsDrawRef.current = true;
      dragNode = -1;
      panning = false;
    };
    const onLeave = () => {
      needsDrawRef.current = true;
      hovered = -1;
    };
    // Double-click a node to focus its FK neighborhood.
    const onDbl = () => {
      if (hovered >= 0) setFocus(nodes[hovered].id);
    };
    cv.addEventListener("wheel", onWheel, { passive: false });
    cv.addEventListener("mousedown", onDown);
    cv.addEventListener("dblclick", onDbl);
    window.addEventListener("mousemove", onMove);
    window.addEventListener("mouseup", onUp);
    cv.addEventListener("mouseleave", onLeave);

    return () => {
      disposed = true;
      cancelAnimationFrame(raf);
      ro.disconnect();
      cv.removeEventListener("wheel", onWheel);
      cv.removeEventListener("mousedown", onDown);
      cv.removeEventListener("dblclick", onDbl);
      window.removeEventListener("mousemove", onMove);
      window.removeEventListener("mouseup", onUp);
      cv.removeEventListener("mouseleave", onLeave);
    };
  }, [graph, hideIsolated, maxNode, focus, hops]);

  const schemas = [
    ...new Set(
      (graph?.nodes ?? []).filter((n): n is GraphNode => n !== null).map((n) => n.schema),
    ),
  ];

  const nodeIds = useMemo(
    () =>
      (graph?.nodes ?? [])
        .filter((n): n is GraphNode => n !== null)
        .map((n) => `${n.schema}.${n.table}`),
    [graph],
  );

  // Ranked fuzzy matches for the finder dropdown (best score first, short
  // ids break ties). Capped — beyond that the query needs more letters.
  const findMatches = useMemo(() => {
    const q = findVal.trim();
    if (!q) return [];
    return nodeIds
      .map((id) => ({ id, m: fuzzyMatch(q, id) }))
      .filter((x): x is { id: string; m: { score: number; pos: number[] } } => x.m !== null)
      .sort((a, b) => b.m.score - a.m.score || a.id.length - b.id.length || a.id.localeCompare(b.id))
      .slice(0, 50);
  }, [findVal, nodeIds]);

  // Accept a full schema.table id or a bare table name when unambiguous.
  const tryFocus = (v: string) => {
    const val = v.trim();
    if (!val) return;
    const suffix = nodeIds.filter((id) => id.endsWith(`.${val}`));
    const hit = nodeIds.includes(val) ? val : suffix.length === 1 ? suffix[0] : findMatches[0]?.id;
    if (hit) {
      setFocus(hit);
      setFindVal("");
      setFindOpen(false);
    }
  };

  return (
    <div className="flex h-full flex-col">
      <div className="flex h-[calc(2.5rem+1px)] shrink-0 items-center gap-3 border-b px-3 text-xs">
        <span className="font-medium">Schema graph</span>
        <Button size="xs" variant="outline" onClick={() => void refresh()}>
          Refresh
        </Button>
        <Button
          size="xs"
          variant="outline"
          data-tip="Zoom to fit the whole graph (auto until you pan or zoom)"
          onClick={() => controls.current.fit?.()}
        >
          Fit
        </Button>
        <label className="flex items-center gap-1.5 text-muted-foreground">
          <Switch checked={hideIsolated} onCheckedChange={setHideIsolated} /> Hide tables without FKs
        </label>
        <label
          className="flex items-center gap-1.5 text-muted-foreground"
          data-tip="Max node size — biggest table gets this radius, the rest scale by row count"
        >
          Node size
          <input
            type="range"
            min={8}
            max={48}
            step={1}
            value={maxNode}
            className="h-1.5 w-28 cursor-pointer appearance-none rounded-full bg-muted-foreground/30 accent-primary"
            onChange={(e) => setMaxNode(Number(e.target.value))}
          />
        </label>
        {/* fzf-style finder: fuzzy dropdown, ↑/↓ select, Tab completes the
            selection into the input, Enter focuses it. A native datalist
            renders no suggestions in the Electron shell. */}
        <div className="relative">
          <input
            placeholder="Find Table…"
            className="h-6 w-40 rounded-md border bg-transparent px-2 text-xs outline-none focus:border-ring"
            value={findVal}
            onChange={(e) => {
              setFindVal(e.target.value);
              setFindSel(0);
              setFindOpen(true);
            }}
            onFocus={() => setFindOpen(true)}
            onBlur={() => setFindOpen(false)}
            onKeyDown={(e) => {
              if (e.key === "ArrowDown" || e.key === "ArrowUp") {
                e.preventDefault();
                const d = e.key === "ArrowDown" ? 1 : -1;
                setFindSel((s) => Math.min(Math.max(s + d, 0), findMatches.length - 1));
              } else if (e.key === "Tab" && findMatches.length > 0) {
                e.preventDefault();
                setFindVal(findMatches[findSel]?.id ?? findMatches[0].id);
                setFindSel(0);
              } else if (e.key === "Enter") {
                tryFocus(findMatches[findSel]?.id ?? findVal);
              } else if (e.key === "Escape") {
                setFindOpen(false);
              }
            }}
          />
          {findOpen && findMatches.length > 0 && (
            <div className="absolute left-0 top-full z-50 mt-1 max-h-72 w-72 overflow-auto rounded-md border bg-popover py-1 shadow-md">
              {findMatches.map(({ id, m }, i) => (
                <div
                  key={id}
                  ref={i === findSel ? (el) => el?.scrollIntoView({ block: "nearest" }) : undefined}
                  className={
                    "cursor-pointer truncate px-2 py-1 font-mono " +
                    (i === findSel ? "bg-accent text-accent-foreground" : "text-muted-foreground")
                  }
                  // mousedown, not click: fires before the input's blur closes
                  // the dropdown.
                  onMouseDown={(e) => {
                    e.preventDefault();
                    tryFocus(id);
                  }}
                  onMouseEnter={() => setFindSel(i)}
                >
                  {id.split("").map((ch, ci) =>
                    m.pos.includes(ci) ? (
                      <span key={ci} className="font-medium text-primary">
                        {ch}
                      </span>
                    ) : (
                      ch
                    ),
                  )}
                </div>
              ))}
            </div>
          )}
        </div>
        {focus && (
          <>
            <span className="max-w-56 truncate text-foreground" title={focus}>
              ◎ {focus}
            </span>
            <label
              className="flex items-center gap-1.5 text-muted-foreground"
              data-tip="How many foreign-key jumps from the focused table stay visible"
            >
              Jumps
              <input
                type="range"
                min={1}
                max={6}
                step={1}
                value={hops}
                className="h-1.5 w-20 cursor-pointer appearance-none rounded-full bg-muted-foreground/30 accent-primary"
                onChange={(e) => setHops(Number(e.target.value))}
              />
              <span className="tabular-nums">{hops}</span>
            </label>
            <Button size="xs" variant="outline" onClick={() => setFocus(null)}>
              Clear Focus
            </Button>
          </>
        )}
        <span className="text-muted-foreground">
          {counts.nodes} tables · {counts.edges} foreign keys
        </span>
        <div className="flex-1" />
        <div className="flex items-center gap-2 overflow-hidden">
          {schemas.slice(0, 8).map((s) => (
            <span key={s} className="flex items-center gap-1 text-muted-foreground">
              <span
                className="h-2 w-2 rounded-full"
                style={{ background: `hsl(${schemaHue(s)} 55% 56%)` }}
              />
              {s}
            </span>
          ))}
        </div>
      </div>
      {error && (
        <pre className="m-2 whitespace-pre-wrap rounded-md border border-destructive/50 bg-destructive/10 p-2 font-mono text-xs text-destructive">
          {error}
        </pre>
      )}
      {graph && counts.edges === 0 && counts.nodes === 0 && (
        <p className="p-3 text-xs text-muted-foreground">No user tables found.</p>
      )}
      <div ref={wrap} className="min-h-0 flex-1 overflow-hidden">
        <canvas ref={canvas} />
      </div>
    </div>
  );
}
