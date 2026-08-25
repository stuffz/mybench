package rpc

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

type svc struct{}

type point struct {
	X int `json:"x"`
	Y int `json:"y"`
}

func (svc) Echo(s string) string        { return s }
func (svc) Add(a, b int) int            { return a + b }
func (svc) Move(p point, dx int) *point { return &point{X: p.X + dx, Y: p.Y} }
func (svc) Fail() (string, error)       { return "", errors.New("boom") }
func (svc) OK() error                   { return nil }
func (svc) Nothing()                    {}
func (svc) Panics() string              { panic("kaboom") }

func call(t *testing.T, srv *Server, path, body, token string) reply {
	t.Helper()
	req := httptest.NewRequestWithContext(context.Background(), http.MethodPost, path,
		strings.NewReader(body))
	if body != "" {
		req.ContentLength = int64(len(body))
	}
	if token != "" {
		req.Header.Set("X-Mybench-Token", token)
	}
	w := httptest.NewRecorder()
	srv.Handler().ServeHTTP(w, req)
	var r reply
	if err := json.Unmarshal(w.Body.Bytes(), &r); err != nil {
		t.Fatalf("decode %q: %v", w.Body.String(), err)
	}
	return r
}

func TestDispatch(t *testing.T) {
	t.Parallel()
	srv := New("")
	srv.Register("s", svc{})

	if got := call(t, srv, "/rpc/s/Echo", `["hi"]`, "").Result; got != "hi" {
		t.Errorf("Echo = %v, want hi", got)
	}
	if got := call(t, srv, "/rpc/s/Add", `[2,3]`, "").Result; got != float64(5) {
		t.Errorf("Add = %v, want 5", got)
	}
	// Structs decode from their JSON tags, the same shape the frontend used.
	moved, ok := call(t, srv, "/rpc/s/Move", `[{"x":1,"y":2},10]`, "").Result.(map[string]any)
	if !ok {
		t.Fatal("Move did not return an object")
	}
	if moved["x"] != float64(11) || moved["y"] != float64(2) {
		t.Errorf("Move = %v, want x=11 y=2", moved)
	}
	// A nil error must not turn into an error reply.
	if r := call(t, srv, "/rpc/s/OK", `[]`, ""); r.Error != "" {
		t.Errorf("OK errored: %q", r.Error)
	}
	if r := call(t, srv, "/rpc/s/Nothing", "", ""); r.Error != "" {
		t.Errorf("Nothing errored: %q", r.Error)
	}
}

func TestErrors(t *testing.T) {
	t.Parallel()
	srv := New("")
	srv.Register("s", svc{})

	if r := call(t, srv, "/rpc/s/Fail", `[]`, ""); r.Error != "boom" {
		t.Errorf("Fail error = %q, want boom", r.Error)
	}
	if r := call(t, srv, "/rpc/s/Nope", `[]`, ""); !strings.Contains(r.Error, "no such method") {
		t.Errorf("unknown method error = %q", r.Error)
	}
	if r := call(t, srv, "/rpc/nope/Echo", `[]`, ""); !strings.Contains(r.Error, "no such service") {
		t.Errorf("unknown service error = %q", r.Error)
	}
	if r := call(t, srv, "/rpc/s/Add", `[1]`, ""); !strings.Contains(r.Error, "wants 2 args") {
		t.Errorf("arity error = %q", r.Error)
	}
	if r := call(t, srv, "/rpc/s/Echo", `[5]`, ""); !strings.Contains(r.Error, "arg 0") {
		t.Errorf("arg type error = %q", r.Error)
	}
	// A panicking service call must not take the backend down with it.
	if r := call(t, srv, "/rpc/s/Panics", `[]`, ""); !strings.Contains(r.Error, "panic") {
		t.Errorf("panic error = %q", r.Error)
	}
}

func TestToken(t *testing.T) {
	t.Parallel()
	srv := New("secret")
	srv.Register("s", svc{})

	req := httptest.NewRequestWithContext(context.Background(), http.MethodPost, "/rpc/s/Echo",
		strings.NewReader(`["hi"]`))
	w := httptest.NewRecorder()
	srv.Handler().ServeHTTP(w, req)
	if w.Code != http.StatusForbidden {
		t.Errorf("no token: status %d, want 403", w.Code)
	}
	if got := call(t, srv, "/rpc/s/Echo", `["hi"]`, "secret").Result; got != "hi" {
		t.Errorf("with token: %v", got)
	}
}

func TestMethodNotAllowed(t *testing.T) {
	t.Parallel()
	srv := New("")
	srv.Register("s", svc{})
	req := httptest.NewRequestWithContext(context.Background(), http.MethodGet, "/rpc/s/Echo",
		http.NoBody)
	w := httptest.NewRecorder()
	srv.Handler().ServeHTTP(w, req)
	if w.Code != http.StatusMethodNotAllowed {
		t.Errorf("GET: status %d, want 405", w.Code)
	}
}
