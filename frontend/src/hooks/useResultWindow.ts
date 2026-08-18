import { useCallback, useEffect, useRef, useState } from "react";
import { QueryService } from "@/lib/api";

// Windowed access to a Go-side result buffer (SPEC.md: result data never
// enters JS state — this hook caches only the windows the grid can see).

export const WINDOW = 200;

type Row = (string | null)[];

export function useResultWindow(
  resultId: string | null,
  rowCount: number,
  version: number,
) {
  const cache = useRef(new Map<number, Row[]>());
  const pending = useRef(new Set<number>());
  const [, bump] = useState(0);

  // New result or server-side reorder (sort) — every cached window is stale.
  useEffect(() => {
    cache.current.clear();
    pending.current.clear();
    bump((n) => n + 1);
  }, [resultId, version]);

  const getRow = useCallback(
    (index: number): Row | null => {
      if (!resultId || index >= rowCount) return null;
      const w = Math.floor(index / WINDOW);
      const cached = cache.current.get(w);
      if (cached) {
        // A tail window fetched mid-stream may be partial; refetch once more
        // rows exist behind it.
        const stale =
          cached.length < WINDOW && w * WINDOW + cached.length < rowCount;
        if (!stale) return cached[index - w * WINDOW] ?? null;
      }
      if (!pending.current.has(w)) {
        pending.current.add(w);
        QueryService.Rows(resultId, w * WINDOW, WINDOW)
          .then((win) => {
            if (win) cache.current.set(w, (win.rows ?? []) as Row[]);
          })
          .catch(() => undefined)
          .finally(() => {
            pending.current.delete(w);
            bump((n) => n + 1);
          });
      }
      return cached ? (cached[index - w * WINDOW] ?? null) : null;
    },
    [resultId, rowCount],
  );

  return getRow;
}
