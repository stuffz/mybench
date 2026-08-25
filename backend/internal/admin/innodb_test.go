package admin

import (
	"strings"
	"testing"
)

// Trimmed from a real 8.4 server: the timestamped header, a section whose
// rule is only three characters ("LOG"), and a transaction list whose lines
// begin with dashes without being rules.
const sample = `
=====================================
2026-08-22 09:14:02 0x7f2c INNODB MONITOR OUTPUT
=====================================
Per second averages calculated from the last 34 seconds
------------
TRANSACTIONS
------------
Trx id counter 12345
---TRANSACTION 422190412305192, not started
0 lock struct(s), heap size 1136
------------------------
LATEST DETECTED DEADLOCK
------------------------
2026-08-22 09:10:00 0x7f2d
*** (1) TRANSACTION:
TRANSACTION 12340, ACTIVE 3 sec starting index read
---
LOG
---
Log sequence number 1010
----------------------------
END OF INNODB MONITOR OUTPUT
============================
`

func TestSplitInnoDBSections(t *testing.T) {
	t.Parallel()

	got := splitInnoDBSections(sample)
	titles := make([]string, 0, len(got))
	for _, s := range got {
		titles = append(titles, s.Title)
	}
	want := []string{
		monitorHeader, "TRANSACTIONS", "LATEST DETECTED DEADLOCK", "LOG",
		"END OF INNODB MONITOR OUTPUT",
	}
	if len(titles) != len(want) {
		t.Fatalf("titles = %v, want %v", titles, want)
	}
	for i := range want {
		if titles[i] != want[i] {
			t.Fatalf("titles = %v, want %v", titles, want)
		}
	}

	// The header's own timestamp survives in the body.
	if body := Section(got, monitorHeader); body == "" ||
		!strings.Contains(body, "2026-08-22 09:14:02") ||
		!strings.Contains(body, "Per second averages") {
		t.Fatalf("monitor header body = %q", body)
	}

	// A line that only looks like a rule stays inside its section.
	if body := Section(got, "TRANSACTIONS"); !strings.Contains(body, "---TRANSACTION 422190412305192") {
		t.Fatalf("transactions body = %q", body)
	}

	if body := Section(got, "LATEST DETECTED DEADLOCK"); !strings.Contains(body, "*** (1) TRANSACTION:") {
		t.Fatalf("deadlock body = %q", body)
	}

	// A section the server did not print reads as absent, not as an error.
	if body := Section(got, "LATEST FOREIGN KEY ERROR"); body != "" {
		t.Fatalf("missing section body = %q, want empty", body)
	}
}

func TestSplitInnoDBSectionsDegrades(t *testing.T) {
	t.Parallel()

	// No rules at all (a future layout, or a truncated read): one section
	// holding the text, never a panic and never a dropped line.
	got := splitInnoDBSections("some unrecognised\nmonitor text\n")
	if len(got) != 1 || got[0].Title != "" || got[0].Body != "some unrecognised\nmonitor text" {
		t.Fatalf("got %#v", got)
	}
	if len(splitInnoDBSections("")) != 0 {
		t.Fatalf("empty text should yield no sections")
	}
}

func TestIsRule(t *testing.T) {
	t.Parallel()

	rules := []string{"---", "=====", "----------------------------   "}
	for _, r := range rules {
		if !isRule(r) {
			t.Fatalf("isRule(%q) = false", r)
		}
	}
	notRules := []string{"", "--", "==", "---TRANSACTION 1", "LOG", "--=--"}
	for _, r := range notRules {
		if isRule(r) {
			t.Fatalf("isRule(%q) = true", r)
		}
	}
}
