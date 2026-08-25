// Package buildinfo carries build-time stamps injected via -ldflags -X by
// the backend build tasks (Taskfile.yml). Ad-hoc `go build` gets "dev" and
// empty strings; the About dialog skips what is not stamped.
package buildinfo

var (
	// Commit is `git describe` at build time; "dev" when not stamped.
	Commit = "dev"
	// Date is the build time, RFC3339 UTC; empty when not stamped.
	Date = ""
	// Version is build/windows/info.json file_version; empty when not stamped.
	Version = ""
)
