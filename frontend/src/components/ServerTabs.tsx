import { X } from "lucide-react";
import { useApp, connColor } from "@/store";

// Top level of the tab tree: one tab per open connection (the "server
// level"); everything else nests under the active server. Tabs are square,
// every tab carries its connection accent as a light bar across the top
// (dimmed when inactive), and the active tab merges into the row below
// (-mb-px, no bottom border). Only open connections appear here — restored
// workspace tabs stay hidden until their connection is opened from the
// connections dialog.

export function ServerTabs() {
  const { saved, openIDs, activeConn, setActiveConn, closeConn } = useApp();

  if (openIDs.length === 0) return null;

  const connName = (id: string) => saved.find((c) => c.id === id)?.name ?? id;

  return (
    <div className="flex items-end gap-1 border-b bg-muted/30 px-2 pt-1.5">
      {openIDs.map((id) => {
        const accent = connColor(saved, id);
        const active = id === activeConn;
        return (
          <div
            key={id}
            className={
              "-mb-px flex h-8 cursor-pointer select-none items-center gap-2 border-x px-3 text-xs " +
              (active
                ? "border-border bg-background font-medium"
                : "border-transparent text-muted-foreground hover:bg-muted/60 hover:text-foreground")
            }
            style={{
              borderTop: `2px solid ${active ? accent : `color-mix(in srgb, ${accent} 40%, transparent)`}`,
            }}
            onClick={() => setActiveConn(id)}
          >
            <span className="h-2 w-2 shrink-0 rounded-full" style={{ background: accent }} />
            {connName(id)}
            <button
              type="button"
              className="-mr-1 rounded p-0.5 text-muted-foreground hover:bg-muted hover:text-foreground"
              data-tip="Disconnect"
              onClick={(e) => {
                e.stopPropagation();
                closeConn(id);
              }}
            >
              <X className="h-3 w-3" />
            </button>
          </div>
        );
      })}
    </div>
  );
}
