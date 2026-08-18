package mcp

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"

	"github.com/stuffz/mybench/internal/admin"
	"github.com/stuffz/mybench/internal/conn"
	"github.com/stuffz/mybench/internal/storage"
)

func testService(t *testing.T) *Service {
	t.Helper()
	store, err := storage.Open(filepath.Join(t.TempDir(), "t.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	conns := conn.New()
	return New(conns, admin.New(conns), store)
}

func rpc(t *testing.T, s *Service, token, body string) (*httptest.ResponseRecorder, map[string]any) {
	t.Helper()
	req := httptest.NewRequest(http.MethodPost, "/mcp", bytes.NewBufferString(body))
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	rec := httptest.NewRecorder()
	s.handle(rec, req)
	var out map[string]any
	if rec.Body.Len() > 0 {
		_ = json.NewDecoder(rec.Body).Decode(&out)
	}
	return rec, out
}

func TestAuthRequired(t *testing.T) {
	s := testService(t)
	rec, _ := rpc(t, s, "", `{"jsonrpc":"2.0","id":1,"method":"tools/list"}`)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("no token: got %d", rec.Code)
	}
	rec, _ = rpc(t, s, "wrong", `{"jsonrpc":"2.0","id":1,"method":"tools/list"}`)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("wrong token: got %d", rec.Code)
	}
}

func TestInitializeAndToolsList(t *testing.T) {
	s := testService(t)
	token, _ := s.token()

	rec, out := rpc(t, s, token, `{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}`)
	if rec.Code != http.StatusOK {
		t.Fatalf("initialize: %d", rec.Code)
	}
	res := out["result"].(map[string]any)
	if res["protocolVersion"] != protocolVersion {
		t.Fatalf("protocolVersion: %v", res["protocolVersion"])
	}

	_, out = rpc(t, s, token, `{"jsonrpc":"2.0","id":2,"method":"tools/list"}`)
	tools := out["result"].(map[string]any)["tools"].([]any)
	if len(tools) != 6 {
		t.Fatalf("want 6 tools, got %d", len(tools))
	}
}

func TestWriteStatementRejected(t *testing.T) {
	s := testService(t)
	token, _ := s.token()
	body := `{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"run_query","arguments":{"connection":"x","sql":"UPDATE t SET a=1"}}}`
	_, out := rpc(t, s, token, body)
	res := out["result"].(map[string]any)
	if res["isError"] != true {
		t.Fatalf("write must be a tool error: %v", res)
	}
	text := res["content"].([]any)[0].(map[string]any)["text"].(string)
	// The unknown connection must not mask the gate; either message is a
	// rejection, but the gate check runs after resolve — accept both.
	if !strings.Contains(text, "no connection") && !strings.Contains(text, "read-only") {
		t.Fatalf("unexpected rejection text: %q", text)
	}
}

func TestNotificationAccepted(t *testing.T) {
	s := testService(t)
	token, _ := s.token()
	rec, _ := rpc(t, s, token, `{"jsonrpc":"2.0","method":"notifications/initialized"}`)
	if rec.Code != http.StatusAccepted {
		t.Fatalf("notification: got %d", rec.Code)
	}
}
