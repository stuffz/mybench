import { useEffect, useState } from "react";
import { Info } from "lucide-react";
import { AdminService } from "@/lib/api";
import type { AppInfo } from "@/lib/api";
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogHeader,
  DialogTitle,
  DialogTrigger,
} from "@/components/ui/dialog";

// About: what mybench is built on. The permissive licenses (MIT/ISC/BSD)
// don't strictly require in-app attribution, but the projects deserve it.

const CREDITS: [name: string, url: string, note: string][] = [
  ["Wails v3", "https://v3.wails.io", "Go Desktop App Framework"],
  ["Go", "https://go.dev", "Backend"],
  ["go-sql-driver/mysql", "https://github.com/go-sql-driver/mysql", "pure-Go MySQL Driver"],
  ["React", "https://react.dev", "UI"],
  ["CodeMirror 6", "https://codemirror.net", "SQL Editor"],
  ["shadcn/ui", "https://ui.shadcn.com", "Component Library"],
  ["Lucide", "https://lucide.dev", "Icons (ISC)"],
  ["Tailwind CSS", "https://tailwindcss.com", "Styling"],
  ["Zustand", "https://zustand.docs.pmnd.rs", "State Management"],
  ["TanStack Virtual", "https://tanstack.com/virtual", "Result Grid Virtualization"],
  ["Vite", "https://vite.dev", "Build Tooling"],
];

// RFC3339 → "yy-mm-dd hh:mm" in local time; the raw string when unparsable.
function fmtBuildDate(iso: string): string {
  const d = new Date(iso);
  if (isNaN(d.getTime())) return iso;
  const p = (n: number) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`;
}

export function AboutDialog() {
  const [info, setInfo] = useState<AppInfo | null>(null);
  useEffect(() => {
    AdminService.AppInfo().then(setInfo).catch(() => {});
  }, []);

  return (
    <Dialog>
      <DialogTrigger className="flex items-center gap-1.5 rounded-md px-2 py-1 text-xs text-muted-foreground hover:bg-muted/50 hover:text-foreground">
        <Info className="h-3.5 w-3.5" />
        About
      </DialogTrigger>
      <DialogContent className="sm:max-w-md">
        <DialogHeader>
          <DialogTitle>mybench</DialogTitle>
          <DialogDescription>
            A fast, minimal MySQL GUI — one keyring-backed connection store, per-tab
            sessions, and result grids that survive millions of rows.
          </DialogDescription>
        </DialogHeader>
        {info && (
          <p className="font-mono text-xs text-muted-foreground">
            build {info.commit}
            {info.date && ` · ${fmtBuildDate(info.date)}`}
          </p>
        )}
        <div className="text-xs">
          <p className="mb-2 text-muted-foreground">Built On:</p>
          <ul className="grid grid-cols-1 gap-1">
            {CREDITS.map(([name, url, note]) => (
              <li key={name} className="flex items-baseline gap-2">
                <a
                  href={url}
                  target="_blank"
                  rel="noreferrer"
                  className="font-medium underline-offset-2 hover:underline"
                >
                  {name}
                </a>
                <span className="text-muted-foreground">{note}</span>
              </li>
            ))}
          </ul>
        </div>
      </DialogContent>
    </Dialog>
  );
}
