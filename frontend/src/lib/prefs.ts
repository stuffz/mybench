// Preference model + the side effects that apply it. Stored inside the
// workspace blob so it survives restarts like everything else.

import { QueryService } from "@/lib/api";

export type Prefs = {
  appTheme: "dark" | "light" | "gruvbox" | "nord" | "solarized";
  editorTheme: "onedark" | "gruvbox" | "nord" | "solarized" | "light";
  uiFont: string;
  monoFont: string;
  uiFontSize: number;
  editorFontSize: number;
  // Indent width, in spaces — the editor's tab size and the SQL formatter's.
  tabSize: number;
  // Hide MySQL's own schemas in the schema tree.
  hideDefaultDBs: boolean;
  // Hard ceiling on rows any fetch may buffer ("Fetch All" included).
  maxRows: number;
  // Query-history entries kept per server; older ones are pruned.
  historyKeep: number;
  // Schema graph: max node radius (the slider on the graph page).
  graphNodeSize: number;
};

export const DEFAULT_PREFS: Prefs = {
  appTheme: "dark",
  editorTheme: "onedark",
  uiFont: "'RobotoMono Nerd Font', monospace",
  monoFont: "'RobotoMono Nerd Font', monospace",
  uiFontSize: 16,
  editorFontSize: 13,
  tabSize: 4,
  hideDefaultDBs: true,
  maxRows: 500_000,
  historyKeep: 10_000,
  graphNodeSize: 16,
};

export const APP_THEMES: [Prefs["appTheme"], string][] = [
  ["dark", "Dark"],
  ["light", "Light"],
  ["gruvbox", "Gruvbox"],
  ["nord", "Nord"],
  ["solarized", "Solarized"],
];

export const EDITOR_THEMES: [Prefs["editorTheme"], string][] = [
  ["onedark", "One Dark"],
  ["gruvbox", "Gruvbox"],
  ["nord", "Nord"],
  ["solarized", "Solarized"],
  ["light", "Light"],
];

// Font choices are stacks: the embedded Nerd Font always exists; the rest
// depend on what the OS has installed and fall back to generic families.
export const UI_FONTS: [string, string][] = [
  ["'RobotoMono Nerd Font', monospace", "RobotoMono Nerd Font (bundled)"],
  ["ui-sans-serif, system-ui, sans-serif", "System sans"],
  ["'Inter', ui-sans-serif, system-ui, sans-serif", "Inter"],
  ["'JetBrains Mono', 'RobotoMono Nerd Font', monospace", "JetBrains Mono"],
];

export const MONO_FONTS: [string, string][] = [
  ["'RobotoMono Nerd Font', monospace", "RobotoMono Nerd Font (bundled)"],
  ["'JetBrains Mono', 'RobotoMono Nerd Font', monospace", "JetBrains Mono"],
  ["'Fira Code', 'RobotoMono Nerd Font', monospace", "Fira Code"],
  ["'Cascadia Code', 'RobotoMono Nerd Font', monospace", "Cascadia Code"],
  ["'Source Code Pro', 'RobotoMono Nerd Font', monospace", "Source Code Pro"],
  ["ui-monospace, monospace", "System mono"],
];

export function applyPrefs(p: Prefs) {
  const el = document.documentElement;
  el.classList.toggle("dark", p.appTheme !== "light");
  if (p.appTheme === "dark" || p.appTheme === "light") {
    delete el.dataset.theme;
  } else {
    el.dataset.theme = p.appTheme;
  }
  el.style.setProperty("--app-font-sans", p.uiFont);
  el.style.setProperty("--app-font-mono", p.monoFont);
  // Everything is rem-based, so the root size scales the whole UI.
  el.style.fontSize = `${p.uiFontSize}px`;
  // History retention lives backend-side; the blob is opaque to Go, so the
  // preference is pushed explicitly (on restore and on every change).
  void QueryService.SetHistoryLimit(p.historyKeep).catch(() => {});
}
