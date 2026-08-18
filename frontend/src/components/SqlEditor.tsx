import { useEffect, useImperativeHandle, useRef, forwardRef } from "react";
import { EditorView, keymap, placeholder as cmPlaceholder } from "@codemirror/view";
import { EditorState, Prec, Compartment } from "@codemirror/state";
import { defaultKeymap, history, historyKeymap } from "@codemirror/commands";
import { autocompletion, completionKeymap, closeBrackets } from "@codemirror/autocomplete";
import { sql, MySQL } from "@codemirror/lang-sql";
import { indentUnit } from "@codemirror/language";
import { format as formatSQL } from "sql-formatter";
import { editorTheme } from "@/lib/editorThemes";
import { buildFuzzySource } from "@/lib/sqlComplete";
import { useApp } from "@/store";

// CodeMirror 6 wrapper. The editor owns its state for the tab's lifetime
// (SPEC.md: undo history survives tab switches because the tab stays
// mounted); the parent pulls the value on demand via the ref.

export type SqlEditorHandle = {
  getValue: () => string;
  // Selection if any, else the semicolon-delimited statement under the cursor.
  getStatementToRun: () => string;
  setValue: (sql: string) => void;
  // Replace the selection (or insert at the cursor) and focus the editor.
  insert: (sql: string) => void;
  focus: () => void;
  format: () => void;
};

type Props = {
  initial?: string;
  schema: Record<string, string[]>;
  onRun: () => void;
  // Debounced document changes — feeds workspace persistence.
  onChange?: (sql: string) => void;
  // Format outcome: an error message, or null on success (clears prior ones).
  onFormatted?: (error: string | null) => void;
};

// Format the selection if any, else the whole buffer. Errors (sql-formatter
// throws on text it cannot parse) leave the document untouched.
function runFormat(
  v: EditorView,
  tabSize: number,
  onFormatted?: (error: string | null) => void,
): boolean {
  const sel = v.state.selection.main;
  const whole = sel.empty;
  const from = whole ? 0 : sel.from;
  const to = whole ? v.state.doc.length : sel.to;
  try {
    const pretty = formatSQL(v.state.sliceDoc(from, to), {
      language: "mysql",
      keywordCase: "upper",
      tabWidth: tabSize,
    });
    v.dispatch({ changes: { from, to, insert: pretty } });
    onFormatted?.(null);
  } catch (e) {
    onFormatted?.(`format: ${e instanceof Error ? e.message : e}`);
  }
  return true;
}

// String/comment-aware statement boundaries — enough for run-at-cursor; the
// backend still executes exactly what it is given. No DELIMITER support (MVP).
function statementAt(text: string, pos: number): string {
  const bounds: number[] = [0];
  let i = 0;
  const n = text.length;
  while (i < n) {
    const c = text[i];
    if (c === "'" || c === '"' || c === "`") {
      const q = c;
      i++;
      while (i < n && text[i] !== q) i += text[i] === "\\" && q !== "`" ? 2 : 1;
      i++;
    } else if (c === "-" && text[i + 1] === "-") {
      while (i < n && text[i] !== "\n") i++;
    } else if (c === "#") {
      while (i < n && text[i] !== "\n") i++;
    } else if (c === "/" && text[i + 1] === "*") {
      i += 2;
      while (i < n && !(text[i] === "*" && text[i + 1] === "/")) i++;
      i += 2;
    } else {
      if (c === ";") bounds.push(i + 1);
      i++;
    }
  }
  bounds.push(n);
  for (let b = bounds.length - 2; b >= 0; b--) {
    if (pos >= bounds[b]) {
      const stmt = text.slice(bounds[b], bounds[b + 1]).replace(/;\s*$/, "");
      return stmt.trim();
    }
  }
  return text.trim();
}

export const SqlEditor = forwardRef<SqlEditorHandle, Props>(function SqlEditor(
  { initial = "", schema, onRun, onChange, onFormatted },
  ref,
) {
  const host = useRef<HTMLDivElement>(null);
  const view = useRef<EditorView | null>(null);
  const onRunRef = useRef(onRun);
  onRunRef.current = onRun;
  const onChangeRef = useRef(onChange);
  onChangeRef.current = onChange;
  const onFormattedRef = useRef(onFormatted);
  onFormattedRef.current = onFormatted;
  const changeTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  // Theme and font size live in compartments so preference changes apply to
  // every mounted editor without recreating it (undo history survives).
  const themeComp = useRef(new Compartment());
  const sizeComp = useRef(new Compartment());
  const tabComp = useRef(new Compartment());
  const themeName = useApp((s) => s.prefs.editorTheme);
  const fontSize = useApp((s) => s.prefs.editorFontSize);
  const tabSize = useApp((s) => s.prefs.tabSize);
  const tabSizeRef = useRef(tabSize);
  tabSizeRef.current = tabSize;
  const prefsAtMount = useRef({ themeName, fontSize, tabSize });
  prefsAtMount.current = { themeName, fontSize, tabSize };

  useEffect(() => {
    if (!host.current) return;
    const state = EditorState.create({
      doc: initial,
      extensions: [
        history(),
        closeBrackets(),
        autocompletion(),
        Prec.highest(
          keymap.of([
            {
              key: "Ctrl-Enter",
              mac: "Cmd-Enter",
              run: () => {
                onRunRef.current();
                return true;
              },
            },
            {
              key: "Ctrl-Shift-f",
              mac: "Cmd-Shift-f",
              run: (v) => runFormat(v, tabSizeRef.current, onFormattedRef.current),
            },
          ]),
        ),
        keymap.of([...defaultKeymap, ...historyKeymap, ...completionKeymap]),
        sql({ dialect: MySQL, schema, upperCaseKeywords: true }),
        // Fuzzy finder across every schema/table/column (Ctrl+Space or typing).
        MySQL.language.data.of({ autocomplete: buildFuzzySource(schema) }),
        themeComp.current.of(editorTheme(prefsAtMount.current.themeName)),
        sizeComp.current.of(
          EditorView.theme({ "&": { fontSize: `${prefsAtMount.current.fontSize}px` } }),
        ),
        tabComp.current.of([
          EditorState.tabSize.of(prefsAtMount.current.tabSize),
          indentUnit.of(" ".repeat(prefsAtMount.current.tabSize)),
        ]),
        cmPlaceholder("SELECT …"),
        EditorView.updateListener.of((u) => {
          if (!u.docChanged || !onChangeRef.current) return;
          if (changeTimer.current) clearTimeout(changeTimer.current);
          changeTimer.current = setTimeout(
            () => onChangeRef.current?.(u.state.doc.toString()),
            300,
          );
        }),
        EditorView.theme({
          "&": { height: "100%" },
          ".cm-scroller": { fontFamily: "var(--app-font-mono)", overflow: "auto" },
          // The bundled Nerd Font has tall metrics: without explicit padding
          // and line-height the first line (and the placeholder, which is
          // vertical-align: top) clips against the top border.
          ".cm-content": { padding: "6px 0", lineHeight: "1.55" },
          ".cm-placeholder": { verticalAlign: "baseline" },
          "&.cm-focused": { outline: "none" },
        }),
      ],
    });
    const v = new EditorView({ state, parent: host.current });
    view.current = v;
    return () => {
      if (changeTimer.current) clearTimeout(changeTimer.current);
      v.destroy();
    };
    // The editor is created once per tab; schema arrives before first use in
    // practice (fetched on connect) and changing it later means tab reopen.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => {
    view.current?.dispatch({
      effects: [
        themeComp.current.reconfigure(editorTheme(themeName)),
        sizeComp.current.reconfigure(
          EditorView.theme({ "&": { fontSize: `${fontSize}px` } }),
        ),
        tabComp.current.reconfigure([
          EditorState.tabSize.of(tabSize),
          indentUnit.of(" ".repeat(tabSize)),
        ]),
      ],
    });
  }, [themeName, fontSize, tabSize]);

  useImperativeHandle(ref, () => ({
    getValue: () => view.current?.state.doc.toString() ?? "",
    getStatementToRun: () => {
      const v = view.current;
      if (!v) return "";
      const sel = v.state.selection.main;
      if (!sel.empty) return v.state.sliceDoc(sel.from, sel.to).trim();
      return statementAt(v.state.doc.toString(), sel.head);
    },
    setValue: (sqlText: string) => {
      const v = view.current;
      if (!v) return;
      v.dispatch({ changes: { from: 0, to: v.state.doc.length, insert: sqlText } });
    },
    insert: (sqlText: string) => {
      const v = view.current;
      if (!v) return;
      const sel = v.state.selection.main;
      v.dispatch({
        changes: { from: sel.from, to: sel.to, insert: sqlText },
        selection: { anchor: sel.from + sqlText.length },
      });
      v.focus();
    },
    focus: () => view.current?.focus(),
    format: () => {
      if (view.current) runFormat(view.current, tabSizeRef.current, onFormattedRef.current);
    },
  }));

  // The parent (EditorTab) owns the height via its editor/results splitter.
  return (
    <div
      ref={host}
      className="h-full overflow-hidden rounded-md border bg-muted/20 [&_.cm-editor]:h-full"
    />
  );
});
