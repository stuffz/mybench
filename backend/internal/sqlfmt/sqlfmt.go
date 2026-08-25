// Package sqlfmt pretty-prints MySQL statements.
//
// The web build formatted in the browser with sql-formatter (npm). A native
// client has no JS runtime, so formatting moved here — one RPC, same
// Ctrl+Shift+F. This is a token-level formatter, not a parser: it uppercases
// keywords, breaks before the major clauses, and indents inside parentheses.
// It is deliberately conservative — it never reorders or drops tokens, so the
// worst case is ugly output, never wrong SQL. Function calls hug their
// parenthesis and keep their arguments inline; conditions hang indented under
// their clause; BETWEEN … AND never breaks. It is still NOT equivalent to
// sql-formatter's output: complex expressions stay on one line.
package sqlfmt

import (
	"strings"
	"unicode"
)

// Service exposes formatting over the RPC layer.
type Service struct{}

// New builds the service. It holds no state.
func New() *Service { return &Service{} }

// Keywords that appear in more than one table below.
const (
	kwFrom       = "FROM"
	kwInto       = "INTO"
	kwJoin       = "JOIN"
	kwBy         = "BY"
	kwAll        = "ALL"
	kwInner      = "INNER"
	kwLeft       = "LEFT"
	kwRight      = "RIGHT"
	kwFull       = "FULL"
	kwCross      = "CROSS"
	kwInsert     = "INSERT"
	kwDelete     = "DELETE"
	kwUnion      = "UNION"
	kwGroup      = "GROUP"
	kwOrder      = "ORDER"
	kwSelect     = "SELECT"
	kwUpdate     = "UPDATE"
	kwAnd        = "AND"
	kwBetween    = "BETWEEN"
	kwDistinct   = "DISTINCT"
	kwCase       = "CASE"
	kwAdd        = "ADD"
	kwTable      = "TABLE"
	kwReferences = "REFERENCES"
)

// Clauses that start a new line at the current indent.
var breakBefore = map[string]bool{
	kwSelect: true, kwFrom: true, "WHERE": true, "HAVING": true, kwUnion: true,
	"LIMIT": true, "OFFSET": true, "VALUES": true, "SET": true, "RETURNING": true,
	kwInsert: true, kwUpdate: true, kwDelete: true, "REPLACE": true, "WITH": true,
	kwJoin: true, kwInner: true, kwLeft: true, kwRight: true, kwFull: true,
	kwCross: true, "STRAIGHT_JOIN": true, "ON": true, kwAnd: true, "OR": true,
	kwOrder: true, kwGroup: true,
}

// Two-word clauses ("GROUP BY") — the second word must not break.
var glueNext = map[string]string{
	kwGroup: kwBy, kwOrder: kwBy, "PARTITION": kwBy,
	kwInner: kwJoin, kwLeft: kwJoin, kwRight: kwJoin, kwFull: kwJoin, kwCross: kwJoin,
	kwInsert: kwInto, kwDelete: kwFrom, kwUnion: kwAll,
}

// Keywords that keep their space before a "(" — `IN (…)`, `VALUES (…)`.
// Any other word hugs its parenthesis, which is what makes CURDATE() and
// IF(a, b, c) read as calls.
var extraSpacedParen = []string{
	"IN", "AS", "NOT", "EXISTS", kwBetween, "THEN", "ELSE", "WHEN", "LIKE", "IS",
	kwAll, kwDistinct, "USING", "INTERVAL", "KEY", "PRIMARY", "UNIQUE", kwReferences,
	kwTable, kwInto, "DEFAULT", kwCase, "END", kwBy, "REGEXP", "FOREIGN", "CONSTRAINT",
	kwAdd, "COLUMN",
}

var spacedParen = buildSpacedParen()

func buildSpacedParen() map[string]bool {
	set := make(map[string]bool, len(extraSpacedParen)+len(breakBefore))
	for _, k := range extraSpacedParen {
		set[k] = true
	}
	for k := range breakBefore {
		set[k] = true
	}
	return set
}

// After these, the next identifier names a table, so ITS parenthesis is a
// column list, not a call: `INSERT INTO t (a, b)`.
var tableContext = map[string]bool{
	kwInto: true, kwTable: true, kwReferences: true, kwUpdate: true, kwJoin: true, kwFrom: true,
}

// SELECT modifiers stay on the SELECT line; the select-list break waits
// them out.
var selectModifiers = map[string]bool{
	kwDistinct: true, kwAll: true, "SQL_CALC_FOUND_ROWS": true, "HIGH_PRIORITY": true,
	"DISTINCTROW": true,
}

// Everything above is a keyword too — buildKeywords unions all of the sets —
// so this list is only the rest of what we uppercase. Deriving the set avoids
// repeating "FROM", "JOIN" and friends.
var extraKeywords = []string{
	"ALTER", "ANALYZE", "ASC", "AUTO_INCREMENT", "CAST", "COLLATE", "CREATE",
	"DESC", "DROP", "DUPLICATE", "EXPLAIN", "FALSE", "FOR", "IF", "IGNORE",
	"INDEX", "NATURAL", "NULL", "OUTER", "RENAME", "SHOW", "TRUE", "USE",
}

var keywords = buildKeywords()

func buildKeywords() map[string]bool {
	set := make(map[string]bool, 96)
	for _, k := range extraKeywords {
		set[k] = true
	}
	for _, k := range extraSpacedParen {
		set[k] = true
	}
	for k := range breakBefore {
		set[k] = true
	}
	for k := range tableContext {
		set[k] = true
	}
	for k := range selectModifiers {
		set[k] = true
	}
	for k, v := range glueNext {
		set[k] = true
		set[v] = true
	}
	return set
}

type kind int

const (
	word kind = iota
	str
	comment
	punct
	number
)

type token struct {
	text  string
	kind  kind
	upper string
}

func isIdentChar(r rune) bool {
	return r == '_' || r == '$' || r == '@' || unicode.IsLetter(r) || unicode.IsDigit(r)
}

// lexLineComment returns the end of a "--" or "#" comment starting at i.
func lexLineComment(s string, i int) int {
	for i < len(s) && s[i] != '\n' {
		i++
	}
	return i
}

// lexBlockComment returns the end of a "/* … */" starting at i.
func lexBlockComment(s string, i int) int {
	j := i + 2
	for j+1 < len(s) && (s[j] != '*' || s[j+1] != '/') {
		j++
	}
	return min(j+2, len(s))
}

// lexQuoted returns the end of a '…', "…" or `…` run starting at i.
func lexQuoted(s string, i int) int {
	q := s[i]
	j := i + 1
	for j < len(s) {
		if s[j] == '\\' && q != '`' {
			j += 2
			continue
		}
		if s[j] == q {
			break
		}
		j++
	}
	return min(j+1, len(s))
}

func lexWord(s string, i int) int {
	j := i
	for j < len(s) && (isIdentChar(rune(s[j])) || s[j] == '.') {
		j++
	}
	return j
}

func lexNumber(s string, i int) int {
	j := i
	for j < len(s) && (unicode.IsDigit(rune(s[j])) || s[j] == '.') {
		j++
	}
	return j
}

func lex(s string) []token {
	var out []token
	i, n := 0, len(s)
	for i < n {
		c := s[i]
		switch {
		case c == ' ' || c == '\t' || c == '\n' || c == '\r':
			i++
		case (c == '-' && i+1 < n && s[i+1] == '-') || c == '#':
			j := lexLineComment(s, i)
			out = append(out, token{text: s[i:j], kind: comment})
			i = j
		case c == '/' && i+1 < n && s[i+1] == '*':
			j := lexBlockComment(s, i)
			out = append(out, token{text: s[i:j], kind: comment})
			i = j
		case c == '\'' || c == '"' || c == '`':
			j := lexQuoted(s, i)
			out = append(out, token{text: s[i:j], kind: str})
			i = j
		case isIdentChar(rune(c)) && !unicode.IsDigit(rune(c)):
			j := lexWord(s, i)
			out = append(out, token{text: s[i:j], kind: word, upper: strings.ToUpper(s[i:j])})
			i = j
		case unicode.IsDigit(rune(c)):
			j := lexNumber(s, i)
			out = append(out, token{text: s[i:j], kind: number})
			i = j
		default:
			out = append(out, token{text: string(c), kind: punct})
			i++
		}
	}
	return out
}

// formatter carries the layout state across tokens.
type formatter struct {
	b           strings.Builder
	indentUnit  string
	depth       int
	parenDepth  int
	atLineStart bool
	// Set by "(" so the token after it hugs the paren.
	noSpace bool
	glue    string // expected second word of a two-word clause
	// After SELECT: the first select-list item starts its own indented line.
	pendingItems bool
	// Between BETWEEN and its AND, which must not line-break.
	afterBetween bool
	// The last word written and the word before that, for the call-vs-clause
	// parenthesis decision.
	lastWasWord bool
	lastUpper   string
	prevUpper   string
}

func (f *formatter) newline() {
	f.noSpace = false
	if f.b.Len() == 0 {
		return
	}
	f.b.WriteByte('\n')
	f.b.WriteString(strings.Repeat(f.indentUnit, f.depth))
	f.atLineStart = true
}

func (f *formatter) space() {
	if f.noSpace {
		f.noSpace = false
		return
	}
	if !f.atLineStart && f.b.Len() > 0 {
		f.b.WriteByte(' ')
	}
}

func (f *formatter) write(s string) {
	f.b.WriteString(s)
	f.atLineStart = false
}

// itemStart breaks before the first select-list item; modifiers like DISTINCT
// stay on the SELECT line and keep the break pending.
func (f *formatter) itemStart(upper string) {
	if !f.pendingItems || selectModifiers[upper] {
		return
	}
	f.pendingItems = false
	f.depth++
	f.newline()
	f.depth--
}

func (f *formatter) punct(text string) {
	switch text {
	case "(":
		f.itemStart("")
		// An identifier (or function keyword) hugs its parenthesis — a call;
		// clause keywords and table names keep the space.
		if f.lastWasWord && !spacedParen[f.lastUpper] && !tableContext[f.prevUpper] {
			f.noSpace = true
		}
		f.space()
		f.write("(")
		f.depth++
		f.parenDepth++
		f.noSpace = true
	case ")":
		f.noSpace = false
		f.depth = max(0, f.depth-1)
		f.parenDepth = max(0, f.parenDepth-1)
		f.write(")")
	case ",":
		// Commas end a line in a top-level list (select list, VALUES rows);
		// inside parentheses, arguments stay on one line.
		f.write(",")
		if f.parenDepth == 0 {
			f.depth++
			f.newline()
			f.depth--
		}
	case ";":
		f.write(";")
		f.depth = 0
		f.parenDepth = 0
		f.pendingItems = false
		f.afterBetween = false
		f.b.WriteByte('\n')
		f.atLineStart = true
	default:
		f.itemStart("")
		f.space()
		f.write(text)
	}
}

// isCondWord reports condition connectors, which hang one level under their
// clause and never break inside parentheses.
func isCondWord(upper string) bool {
	return upper == kwAnd || upper == "OR" || upper == "ON"
}

// breaksLine decides whether the keyword starts a new line here.
func (f *formatter) breaksLine(t token, first bool) bool {
	if first || !keywords[t.upper] || !breakBefore[t.upper] {
		return false
	}
	if t.upper == kwAnd && f.afterBetween {
		return false // BETWEEN a AND b is one expression
	}
	if f.parenDepth > 0 && isCondWord(t.upper) {
		return false // parenthesised conditions stay on their line
	}
	return true
}

func (f *formatter) word(t token, first bool) {
	// The second half of a two-word clause never starts a line.
	if f.glue != "" && t.upper == f.glue {
		f.glue = ""
		f.space()
		f.write(t.upper)
		return
	}
	f.glue = ""

	isKeyword := keywords[t.upper]
	switch {
	case f.breaksLine(t, first):
		f.pendingItems = false
		// Conditions hang one level under the clause they belong to: ON and
		// AND under their JOIN (or WHERE).
		if isCondWord(t.upper) {
			f.depth++
			f.newline()
			f.depth--
		} else {
			f.newline()
		}
	case !isKeyword || !breakBefore[t.upper]:
		f.itemStart(t.upper)
		f.space()
	default:
		f.space() // a clause keyword as the very first token
	}
	f.afterBetween = f.afterBetween && t.upper != kwAnd

	if !isKeyword {
		f.write(t.text)
		return
	}
	f.write(t.upper)
	switch t.upper {
	case kwSelect:
		if f.parenDepth == 0 {
			f.pendingItems = true
		}
	case kwBetween:
		f.afterBetween = true
	}
	if next, ok := glueNext[t.upper]; ok {
		f.glue = next
	}
}

// Format lays out one or more statements. tabWidth is the indent in spaces
// (the editor's tab size); 0 means 4.
func (s *Service) Format(sqlText string, tabWidth int) (string, error) {
	if tabWidth <= 0 {
		tabWidth = 4
	}
	f := &formatter{indentUnit: strings.Repeat(" ", tabWidth), atLineStart: true}

	for i, t := range lex(sqlText) {
		switch t.kind {
		case comment:
			f.newline()
			f.write(t.text)
			f.newline()
		case punct:
			f.punct(t.text)
		case word:
			f.word(t, i == 0)
		case str, number:
			f.itemStart("")
			f.space()
			f.write(t.text)
		}
		if t.kind == word {
			f.prevUpper = f.lastUpper
			f.lastUpper = t.upper
			f.lastWasWord = true
		} else {
			f.prevUpper = ""
			f.lastUpper = ""
			f.lastWasWord = false
		}
	}

	out := strings.TrimSpace(f.b.String())
	// Collapse the blank lines a trailing comment or ";" can leave behind.
	for strings.Contains(out, "\n\n\n") {
		out = strings.ReplaceAll(out, "\n\n\n", "\n\n")
	}
	return out, nil
}
