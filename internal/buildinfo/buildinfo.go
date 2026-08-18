// Package buildinfo carries build-time stamps injected via -ldflags -X
// (see build:server in build/Taskfile.yml). Ad-hoc `go build` gets "dev".
package buildinfo

var (
	Commit = "dev"
	Date   = "" // RFC3339 UTC; empty when not stamped
)
