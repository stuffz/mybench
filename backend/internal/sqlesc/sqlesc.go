// Package sqlesc is the single home for MySQL quoting — identifiers and
// string literals. Every generated statement (edit previews, MCP, SHOW
// GRANTS) quotes through here so the escaping rules can't diverge.
package sqlesc

import "strings"

// Ident quotes an identifier with backticks (“ doubles embedded ones).
func Ident(s string) string {
	return "`" + strings.ReplaceAll(s, "`", "``") + "`"
}

var valueReplacer = strings.NewReplacer(
	`\`, `\\`,
	`'`, `''`,
	"\x00", `\0`,
	"\n", `\n`,
	"\r", `\r`,
	"\x1a", `\Z`,
)

// Value renders s as a single-quoted MySQL string literal. The server
// coerces string literals to the column type, so every generated value can
// cross as a quoted string.
func Value(s string) string {
	return "'" + valueReplacer.Replace(s) + "'"
}

// NullableValue renders a *string, mapping nil to NULL.
func NullableValue(v *string) string {
	if v == nil {
		return "NULL"
	}
	return Value(*v)
}
