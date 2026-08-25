package admin

// The one part of SHOW ENGINE INNODB STATUS worth parsing: its section
// boundaries. LATEST DETECTED DEADLOCK, LATEST FOREIGN KEY ERROR and
// SEMAPHORES have no structured equivalent anywhere in information_schema, so
// the dashboard shows those verbatim; everything else it needs comes from
// INNODB_METRICS (see dashboard.go).
//
// The split is deliberately shallow — titles and bodies, no field extraction.
// The monitor text is not a stable interface, so an unrecognised layout
// degrades to fewer sections with their text intact rather than to wrong
// numbers.

import (
	"context"
	"fmt"
	"strings"
)

// InnoDBSection is one titled block of the monitor output.
type InnoDBSection struct {
	Title string `json:"title"`
	Body  string `json:"body"`
}

// monitorHeader is the title InnoDB stamps with the sample time; it is
// reported under a stable name with the original line kept in the body.
const monitorHeader = "INNODB MONITOR OUTPUT"

// InnoDBSections returns the monitor text split into its titled sections.
func (s *Service) InnoDBSections(connID string) ([]InnoDBSection, error) {
	db, err := s.pool(connID)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), adminTimeout)
	defer cancel()

	var typ, name, status string
	if err := db.QueryRowContext(ctx, "SHOW ENGINE INNODB STATUS").
		Scan(&typ, &name, &status); err != nil {
		return nil, fmt.Errorf("innodb status: %w", err)
	}
	return splitInnoDBSections(status), nil
}

// isRule reports whether a line is one of the ---- / ==== rules InnoDB draws
// above and below every section title.
func isRule(line string) bool {
	s := strings.TrimRight(line, " \t")
	if len(s) < 3 {
		return false
	}
	c := s[0]
	if c != '-' && c != '=' {
		return false
	}
	return strings.Trim(s, string(c)) == ""
}

// splitInnoDBSections splits on the rule/title/rule pattern. Body lines that
// merely start with dashes ("---TRANSACTION 42, not started") are not rules,
// so the transaction list stays inside its section.
func splitInnoDBSections(text string) []InnoDBSection {
	lines := strings.Split(strings.ReplaceAll(text, "\r\n", "\n"), "\n")

	var out []InnoDBSection
	title := ""
	var body []string
	flush := func() {
		joined := strings.Trim(strings.Join(body, "\n"), "\n")
		body = nil
		if title == "" && joined == "" {
			return
		}
		out = append(out, InnoDBSection{Title: title, Body: joined})
	}

	i := 0
	for i < len(lines) {
		if i+2 < len(lines) && isRule(lines[i]) && isRule(lines[i+2]) &&
			strings.TrimSpace(lines[i+1]) != "" && !isRule(lines[i+1]) {
			flush()
			title = strings.TrimSpace(lines[i+1])
			// "END OF INNODB MONITOR OUTPUT" ends with the same words and
			// is a section of its own, not the timestamped header.
			if strings.HasSuffix(title, monitorHeader) && !strings.HasPrefix(title, "END OF") {
				// Keep the timestamp the header carries, under a title the
				// client can look up.
				body = append(body, title)
				title = monitorHeader
			}
			i += 3
			continue
		}
		body = append(body, lines[i])
		i++
	}
	flush()
	return out
}

// Section returns one section's body by title, empty when the server did not
// print it (no deadlock since startup, for instance).
func Section(sections []InnoDBSection, title string) string {
	for _, s := range sections {
		if strings.EqualFold(s.Title, title) {
			return s.Body
		}
	}
	return ""
}
