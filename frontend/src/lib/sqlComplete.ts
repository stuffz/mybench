import type {
  Completion,
  CompletionContext,
  CompletionResult,
} from "@codemirror/autocomplete";

// Global fuzzy completion across schemas, tables and columns. CodeMirror's
// autocomplete already ranks with an fzf-style subsequence matcher — this
// source just puts every object into the pool, while lang-sql's contextual
// source keeps handling the `schema.` / `table.` dot cases.

export function buildFuzzySource(
  schema: Record<string, string[]>,
): (ctx: CompletionContext) => CompletionResult | null {
  const options: Completion[] = [];
  const schemas = new Set<string>();
  // Column dedupe: same column name in many tables becomes one entry with
  // the first location as detail (+n more) — keeps the pool small on wide
  // prod schemas.
  const columns = new Map<string, { first: string; count: number }>();

  for (const key of Object.keys(schema)) {
    const dot = key.indexOf(".");
    if (dot < 0) continue; // qualified keys are canonical; bare ones duplicate them
    const sch = key.slice(0, dot);
    const table = key.slice(dot + 1);
    if (!schemas.has(sch)) {
      schemas.add(sch);
      options.push({ label: sch, type: "namespace", detail: "schema", boost: 2 });
    }
    options.push({ label: table, type: "class", detail: sch, boost: 1 });
    options.push({ label: key, type: "class", boost: -1 });
    for (const col of schema[key]) {
      const cur = columns.get(col);
      if (cur) {
        cur.count++;
      } else {
        columns.set(col, { first: key, count: 1 });
      }
    }
  }
  for (const [col, { first, count }] of columns) {
    options.push({
      label: col,
      type: "property",
      detail: count > 1 ? `${first} +${count - 1} more` : first,
      boost: -2,
    });
  }

  return (ctx: CompletionContext) => {
    // Dotted paths belong to lang-sql's contextual source.
    if (ctx.matchBefore(/\.[\w]*/)) return null;
    const word = ctx.matchBefore(/\w+/);
    if (!word && !ctx.explicit) return null;
    return {
      from: word ? word.from : ctx.pos,
      options,
      validFor: /^\w*$/,
    };
  };
}
