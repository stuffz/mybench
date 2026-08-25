// Package host wires every RPC service and serves them over localhost HTTP.
// It is the shared core of cmd/mybench-backend (the spawned exe) and
// cmd/mybench-backend-lib (the c-archive linked into the Windows client), so
// a service registered here reaches both without either drifting.
package host

import (
	"context"
	"errors"
	"fmt"
	"log"
	"net"
	"net/http"
	"sync"
	"time"

	"github.com/stuffz/mybench/internal/admin"
	"github.com/stuffz/mybench/internal/conn"
	"github.com/stuffz/mybench/internal/mcp"
	"github.com/stuffz/mybench/internal/query"
	"github.com/stuffz/mybench/internal/rpc"
	"github.com/stuffz/mybench/internal/sqlfmt"
	"github.com/stuffz/mybench/internal/storage"
	"github.com/stuffz/mybench/internal/workspace"
)

// Host is one running backend: services, listener, HTTP server.
type Host struct {
	conns   *conn.Service
	srv     *http.Server
	addr    string
	done    chan struct{}
	dropped sync.Once
}

// Start wires the services, listens on addr and serves in the background.
func Start(addr, token string) (*Host, error) {
	conns := conn.New()
	store, err := storage.Open(storage.DefaultPath())
	if err != nil {
		return nil, fmt.Errorf("open storage: %w", err)
	}
	adminSvc := admin.New(conns)

	srv := rpc.New(token)
	srv.Register("conn", conns)
	srv.Register("query", query.New(conns, store))
	srv.Register("admin", adminSvc)
	srv.Register("workspace", workspace.New(store))
	srv.Register("mcp", mcp.New(conns, adminSvc, store))
	// Formatting moved backend-side with the native client (no JS runtime).
	srv.Register("sqlfmt", sqlfmt.New())

	// ListenConfig rather than net.Listen so the listen is context-scoped.
	var lc net.ListenConfig
	ln, err := lc.Listen(context.Background(), "tcp", addr)
	if err != nil {
		return nil, fmt.Errorf("listen: %w", err)
	}

	h := &Host{
		conns: conns,
		srv: &http.Server{
			Handler: srv.Handler(),
			// Localhost only, but an unbounded header read is still a hang.
			ReadHeaderTimeout: 10 * time.Second,
		},
		addr: ln.Addr().String(),
		done: make(chan struct{}),
	}
	go func() {
		if err := h.srv.Serve(ln); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Printf("serve: %v", err)
		}
		close(h.done)
	}()
	return h, nil
}

// Addr is the bound listen address, e.g. "127.0.0.1:52341".
func (h *Host) Addr() string { return h.addr }

// Wait blocks until the server stops, then drops tunnels and pools.
func (h *Host) Wait() {
	<-h.done
	h.drop()
}

// Stop shuts the server down gracefully and drops tunnels and pools.
func (h *Host) Stop() {
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_ = h.srv.Shutdown(ctx)
	<-h.done
	h.drop()
}

// Tunnels and pools are process-scoped; drop them explicitly so a killed
// client never leaves an ssh child or an open pool behind.
func (h *Host) drop() {
	h.dropped.Do(func() {
		if err := h.conns.ServiceShutdown(); err != nil {
			log.Printf("shutdown: %v", err)
		}
	})
}
