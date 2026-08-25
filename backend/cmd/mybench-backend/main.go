// mybench-backend is the headless half of the app: every service the GUI
// needs, exposed over localhost HTTP/JSON (internal/rpc). The wiring lives in
// internal/host, shared with cmd/mybench-backend-lib (the single-exe Windows
// build); this entry point adds the process transport around it.
//
// It carries no GUI dependency at all — pure Go, CGO_ENABLED=0 — so it keeps
// the existing trivial cross-compile while the Qt client is built per target.
// The native client spawns it, reads the chosen port from stdout, and holds
// its stdin open: closing stdin (parent gone) shuts the backend down, so
// SSH tunnels and pools never outlive the window.
package main

import (
	"flag"
	"io"
	"log"
	"os"
	"runtime"
	"strings"

	"github.com/stuffz/mybench/internal/host"
)

// Finder/Spotlight launches inherit launchd's minimal PATH, not the login
// shell's — tsh must still be findable when the GUI (and so this process)
// starts that way.
func fixDarwinPath() {
	path := os.Getenv("PATH")
	for _, dir := range []string{"/opt/homebrew/bin", "/usr/local/bin"} {
		if !strings.Contains(":"+path+":", ":"+dir+":") {
			path += ":" + dir
		}
	}
	_ = os.Setenv("PATH", path)
}

func main() {
	addr := flag.String("addr", "127.0.0.1:0", "listen address; port 0 picks a free one")
	watchStdin := flag.Bool("watch-stdin", false, "exit when stdin closes (parent process gone)")
	flag.Parse()

	if runtime.GOOS == "darwin" {
		fixDarwinPath()
	}

	h, err := host.Start(*addr, os.Getenv("MYBENCH_TOKEN"))
	if err != nil {
		log.Fatal(err)
	}
	// The client parses this line to learn the port; keep the format stable.
	_, _ = os.Stdout.WriteString("mybench-backend listening " + h.Addr() + "\n")
	_ = os.Stdout.Sync()

	if *watchStdin {
		go func() {
			_, _ = io.Copy(io.Discard, os.Stdin)
			log.Print("stdin closed — shutting down")
			h.Stop()
		}()
	}

	h.Wait()
}
