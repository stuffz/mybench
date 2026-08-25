// mybench-backend-lib exposes the backend as a C archive so the Windows
// client links it in and ships as a single exe (Taskfile gui:build:windows).
// Same wiring as cmd/mybench-backend via internal/host; only the transport to
// the parent differs — exported calls instead of a stdout line and a held
// stdin. gui/src/app/backend.cpp declares these signatures by hand; keep the
// two in sync.
package main

// #include <stdlib.h>
import "C"

import "github.com/stuffz/mybench/internal/host"

var h *host.Host

// mybenchBackendStart wires and starts the backend on a free localhost port.
// On success it stores the bound address ("127.0.0.1:52341") in *addrOut and
// returns NULL; on failure it returns the error message. Either string is
// malloc'd — the caller frees it.
//
//export mybenchBackendStart
func mybenchBackendStart(token *C.char, addrOut **C.char) *C.char {
	started, err := host.Start("127.0.0.1:0", C.GoString(token))
	if err != nil {
		return C.CString(err.Error())
	}
	h = started
	*addrOut = C.CString(started.Addr())
	return nil
}

// mybenchBackendStop shuts the server down and drops SSH tunnels and pools —
// what closing stdin does for the spawned backend.
//
//export mybenchBackendStop
func mybenchBackendStop() {
	if h != nil {
		h.Stop()
	}
}

func main() {} // required by c-archive; never runs
