// Package buildinfo carries build-time stamps injected via -ldflags -X
// (see build:server in build/Taskfile.yml). Ad-hoc `go build` gets "dev".
//
// resources_windows_amd64.syso (icon, version info, manifest — regenerated
// by windows:generate:syso) also lives here: a .syso is only linked when it
// sits in a compiled package, and this package is in every build, so all
// Windows exes get the icon even from a plain `go build`.
package buildinfo

var (
	Commit = "dev"
	Date   = "" // RFC3339 UTC; empty when not stamped
)
