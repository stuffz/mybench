import type { MouseEvent as ReactMouseEvent } from "react";

// Shared pointer-drag helper for splitters: locks cursor/selection for the
// drag's duration and reports deltas from the mousedown origin.

export function startDrag(
  e: ReactMouseEvent,
  cursor: string,
  onMove: (dx: number, dy: number) => void,
) {
  e.preventDefault();
  const sx = e.clientX;
  const sy = e.clientY;
  document.body.style.cursor = cursor;
  document.body.style.userSelect = "none";
  const move = (ev: MouseEvent) => onMove(ev.clientX - sx, ev.clientY - sy);
  const up = () => {
    document.body.style.cursor = "";
    document.body.style.userSelect = "";
    window.removeEventListener("mousemove", move);
    window.removeEventListener("mouseup", up);
  };
  window.addEventListener("mousemove", move);
  window.addEventListener("mouseup", up);
}
