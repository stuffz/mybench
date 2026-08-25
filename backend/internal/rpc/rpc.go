// Package rpc exposes the services over localhost HTTP/JSON for the GUI.
//
// One generic reflective dispatcher instead of ~50 hand-written handlers: the
// service methods carry JSON tags, so the wire shape falls out of the types
// and stays in lockstep with the services — a new method needs no work here.
// The shape predates this client (the retired web build spoke it), which is
// why method names and JSON fields must stay stable.
package rpc

import (
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"reflect"
	"strings"
)

// Server dispatches POST /rpc/{Service}/{Method} with a JSON array body onto
// registered service values.
type Server struct {
	svcs  map[string]reflect.Value
	token string // when set, required in X-Mybench-Token
}

// New builds a dispatcher. A non-empty token is required in X-Mybench-Token
// on every call; the native client passes one when it spawns the backend.
func New(token string) *Server {
	return &Server{svcs: map[string]reflect.Value{}, token: token}
}

// Register binds a service under the name the client uses ("query", "admin").
func (s *Server) Register(name string, svc any) {
	s.svcs[name] = reflect.ValueOf(svc)
}

type reply struct {
	Result any    `json:"result,omitempty"`
	Error  string `json:"error,omitempty"`
}

var errType = reflect.TypeOf((*error)(nil)).Elem()

// Handler returns the mux serving /rpc/{service}/{method} plus /health.
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/rpc/", s.serve)
	// Liveness — the native client waits on this before showing a window.
	mux.HandleFunc("/health", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte("ok"))
	})
	return mux
}

func (s *Server) serve(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	if s.token != "" && r.Header.Get("X-Mybench-Token") != s.token {
		http.Error(w, "bad token", http.StatusForbidden)
		return
	}
	parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/rpc/"), "/")
	if len(parts) != 2 || parts[0] == "" || parts[1] == "" {
		http.Error(w, "want /rpc/{service}/{method}", http.StatusBadRequest)
		return
	}
	svc, ok := s.svcs[parts[0]]
	if !ok {
		writeJSON(w, reply{Error: "no such service: " + parts[0]})
		return
	}
	m := svc.MethodByName(parts[1])
	if !m.IsValid() {
		writeJSON(w, reply{Error: "no such method: " + parts[0] + "." + parts[1]})
		return
	}

	// Args arrive as a JSON array, positionally. An empty body means none.
	var raw []json.RawMessage
	if r.ContentLength != 0 {
		if err := json.NewDecoder(r.Body).Decode(&raw); err != nil {
			writeJSON(w, reply{Error: "decode args: " + err.Error()})
			return
		}
	}
	mt := m.Type()
	if mt.NumIn() != len(raw) {
		writeJSON(w, reply{Error: fmt.Sprintf("%s.%s wants %d args, got %d", parts[0], parts[1], mt.NumIn(), len(raw))})
		return
	}
	args := make([]reflect.Value, mt.NumIn())
	for i := range args {
		p := reflect.New(mt.In(i))
		if err := json.Unmarshal(raw[i], p.Interface()); err != nil {
			writeJSON(w, reply{Error: fmt.Sprintf("arg %d: %v", i, err)})
			return
		}
		args[i] = p.Elem()
	}

	// A panic in a service call must not take the backend down with it —
	// the client would lose every open connection with it.
	defer func() {
		if p := recover(); p != nil {
			writeJSON(w, reply{Error: fmt.Sprintf("panic: %v", p)})
		}
	}()

	out := m.Call(args)
	res := reply{}
	for _, o := range out {
		if o.Type().Implements(errType) {
			if !o.IsNil() {
				err, ok := o.Interface().(error)
				if !ok {
					err = errors.New("call failed")
				}
				res.Error = err.Error()
				res.Result = nil
				writeJSON(w, res)
				return
			}
			continue
		}
		res.Result = o.Interface()
	}
	writeJSON(w, res)
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(v)
}
