import type { Extension } from "@codemirror/state";
import { EditorView } from "@codemirror/view";
import { HighlightStyle, syntaxHighlighting } from "@codemirror/language";
import { tags as t } from "@lezer/highlight";
import { oneDark } from "@codemirror/theme-one-dark";
import type { Prefs } from "@/lib/prefs";

// CodeMirror themes for the preference palettes, built from small palette
// specs — a full theme package per palette isn't warranted for SQL.

type Palette = {
  dark: boolean;
  bg: string;
  fg: string;
  caret: string;
  sel: string;
  line: string;
  panel: string;
  comment: string;
  keyword: string;
  string: string;
  number: string;
  func: string;
  type: string;
};

function make(c: Palette): Extension {
  const chrome = EditorView.theme(
    {
      "&": { backgroundColor: c.bg, color: c.fg },
      ".cm-content": { caretColor: c.caret },
      ".cm-cursor, .cm-dropCursor": { borderLeftColor: c.caret },
      "&.cm-focused > .cm-scroller > .cm-selectionLayer .cm-selectionBackground, .cm-selectionBackground, ::selection":
        { backgroundColor: c.sel },
      ".cm-activeLine": { backgroundColor: c.line },
      ".cm-gutters": { backgroundColor: c.bg, color: c.comment, border: "none" },
      ".cm-activeLineGutter": { backgroundColor: c.line },
      ".cm-tooltip": { backgroundColor: c.panel, color: c.fg, border: `1px solid ${c.sel}` },
      ".cm-tooltip-autocomplete ul li[aria-selected]": {
        backgroundColor: c.sel,
        color: c.fg,
      },
      ".cm-placeholder": { color: c.comment },
    },
    { dark: c.dark },
  );
  const hl = HighlightStyle.define([
    { tag: t.keyword, color: c.keyword },
    { tag: [t.string, t.special(t.string)], color: c.string },
    { tag: [t.number, t.bool, t.null], color: c.number },
    { tag: t.comment, color: c.comment, fontStyle: "italic" },
    { tag: [t.operator, t.punctuation], color: c.fg },
    { tag: [t.typeName, t.className], color: c.type },
    { tag: [t.function(t.variableName), t.function(t.propertyName)], color: c.func },
    { tag: t.propertyName, color: c.type },
  ]);
  return [chrome, syntaxHighlighting(hl)];
}

const gruvbox = make({
  dark: true,
  bg: "#282828",
  fg: "#ebdbb2",
  caret: "#ebdbb2",
  sel: "#504945",
  line: "#3c383680",
  panel: "#3c3836",
  comment: "#928374",
  keyword: "#fb4934",
  string: "#b8bb26",
  number: "#d3869b",
  func: "#fabd2f",
  type: "#83a598",
});

const nord = make({
  dark: true,
  bg: "#2e3440",
  fg: "#d8dee9",
  caret: "#d8dee9",
  sel: "#434c5e",
  line: "#3b425280",
  panel: "#3b4252",
  comment: "#616e88",
  keyword: "#81a1c1",
  string: "#a3be8c",
  number: "#b48ead",
  func: "#88c0d0",
  type: "#8fbcbb",
});

const solarized = make({
  dark: true,
  bg: "#002b36",
  fg: "#93a1a1",
  caret: "#93a1a1",
  sel: "#073642",
  line: "#07364280",
  panel: "#073642",
  comment: "#586e75",
  keyword: "#859900",
  string: "#2aa198",
  number: "#d33682",
  func: "#b58900",
  type: "#268bd2",
});

const light = make({
  dark: false,
  bg: "#ffffff",
  fg: "#24292e",
  caret: "#24292e",
  sel: "#b3d7ff",
  line: "#f6f8fa",
  panel: "#f6f8fa",
  comment: "#6a737d",
  keyword: "#d73a49",
  string: "#032f62",
  number: "#005cc5",
  func: "#6f42c1",
  type: "#22863a",
});

export function editorTheme(name: Prefs["editorTheme"]): Extension {
  switch (name) {
    case "gruvbox":
      return gruvbox;
    case "nord":
      return nord;
    case "solarized":
      return solarized;
    case "light":
      return light;
    default:
      return oneDark;
  }
}
