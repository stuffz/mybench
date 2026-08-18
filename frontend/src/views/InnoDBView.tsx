import { useCallback, useEffect, useRef, useState } from "react";
import { AdminService } from "@/lib/api";
import { Button } from "@/components/ui/button";
import { useApp } from "@/store";

// SHOW ENGINE INNODB STATUS as a page: the monitor text split on its own
// "--- TITLE ---" section markers, with a section nav on the left and the
// text otherwise shown verbatim — the format is version-dependent, so no
// deeper parsing.

type Props = { connID: string; active: boolean };

type Section = { title: string; body: string };

function parseSections(text: string): Section[] {
  const lines = text.split("\n");
  const sections: Section[] = [];
  let title = "HEADER";
  let body: string[] = [];
  const isRule = (s: string) => /^-{3,}$/.test(s.trim());
  for (let i = 0; i < lines.length; i++) {
    if (
      isRule(lines[i]) &&
      i + 2 < lines.length &&
      lines[i + 1].trim() !== "" &&
      !isRule(lines[i + 1]) &&
      isRule(lines[i + 2])
    ) {
      sections.push({ title, body: body.join("\n").trim() });
      title = lines[i + 1].trim();
      body = [];
      i += 2;
    } else {
      body.push(lines[i]);
    }
  }
  sections.push({ title, body: body.join("\n").trim() });
  return sections.filter((s) => s.body !== "" || s.title !== "HEADER");
}

export function InnoDBView({ connID, active }: Props) {
  const [sections, setSections] = useState<Section[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const refs = useRef<Map<string, HTMLElement>>(new Map());

  const refresh = useCallback(async () => {
    try {
      const text = await AdminService.InnoDBStatus(connID);
      setSections(parseSections(text));
      setError(null);
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }, [connID]);

  const connected = useApp((s) => s.openIDs.includes(connID));
  useEffect(() => {
    if (active && connected && !sections) void refresh();
  }, [active, connected, sections, refresh]);

  return (
    <div className="flex h-full flex-col">
      <div className="flex h-[calc(2.5rem+1px)] shrink-0 items-center gap-3 border-b px-3 text-xs">
        <span className="font-medium">InnoDB Status</span>
        <Button size="xs" variant="outline" onClick={() => void refresh()}>
          Refresh
        </Button>
        <span className="text-muted-foreground">SHOW ENGINE INNODB STATUS</span>
      </div>

      {error && (
        <pre className="m-2 whitespace-pre-wrap rounded-md border border-destructive/50 bg-destructive/10 p-2 font-mono text-xs text-destructive">
          {error}
        </pre>
      )}

      {sections && (
        <div className="flex min-h-0 flex-1">
          <nav className="w-56 shrink-0 overflow-auto border-r p-1 text-xs">
            {sections.map((s) => (
              <button
                key={s.title}
                type="button"
                className="block w-full truncate rounded px-2 py-1 text-left text-muted-foreground hover:bg-muted/50 hover:text-foreground"
                title={s.title}
                onClick={() =>
                  refs.current.get(s.title)?.scrollIntoView({ block: "start" })
                }
              >
                {s.title}
              </button>
            ))}
          </nav>
          <div className="min-h-0 flex-1 overflow-auto p-3">
            {sections.map((s) => (
              <section
                key={s.title}
                ref={(el) => {
                  if (el) refs.current.set(s.title, el);
                  else refs.current.delete(s.title);
                }}
                className="mb-4 scroll-mt-2"
              >
                <h3 className="mb-1 text-xs font-medium">{s.title}</h3>
                <pre className="overflow-x-auto rounded-md border bg-muted/20 p-2 font-mono text-xs leading-relaxed">
                  {s.body}
                </pre>
              </section>
            ))}
          </div>
        </div>
      )}
    </div>
  );
}
