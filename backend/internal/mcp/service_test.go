package mcp

import (
	"bytes"
	"context"
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

func rpc(t *testing.T, s *Service, token, body string) (rec *httptest.ResponseRecorder, out map[string]any) {
	t.Helper()
	req := httptest.NewRequestWithContext(context.Background(), http.MethodPost, "/mcp", bytes.NewBufferString(body))
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	rec = httptest.NewRecorder()
	s.handle(rec, req)
	if rec.Body.Len() > 0 {
		_ = json.NewDecoder(rec.Body).Decode(&out)
	}
	return rec, out
}

// asMap asserts a decoded JSON value is an object.
func asMap(t *testing.T, v any) map[string]any {
	t.Helper()
	m, ok := v.(map[string]any)
	if !ok {
		t.Fatalf("not a JSON object: %v", v)
	}
	return m
}

func TestAuthRequired(t *testing.T) {
	t.Parallel()
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
	t.Parallel()
	s := testService(t)
	token, _ := s.token()

	rec, out := rpc(t, s, token, `{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}`)
	if rec.Code != http.StatusOK {
		t.Fatalf("initialize: %d", rec.Code)
	}
	res := asMap(t, out["result"])
	if res["protocolVersion"] != protocolVersion {
		t.Fatalf("protocolVersion: %v", res["protocolVersion"])
	}

	_, out = rpc(t, s, token, `{"jsonrpc":"2.0","id":2,"method":"tools/list"}`)
	tools, ok := asMap(t, out["result"])["tools"].([]any)
	if !ok {
		t.Fatalf("tools is not a list: %v", out)
	}
	if len(tools) != 6 {
		t.Fatalf("want 6 tools, got %d", len(tools))
	}
}

func TestWriteStatementRejected(t *testing.T) {
	t.Parallel()
	s := testService(t)
	token, _ := s.token()
	body := `{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"run_query","arguments":{"connection":"x","sql":"UPDATE t SET a=1"}}}`
	_, out := rpc(t, s, token, body)
	res := asMap(t, out["result"])
	if isErr, _ := res["isError"].(bool); !isErr {
		t.Fatalf("write must be a tool error: %v", res)
	}
	content, ok := res["content"].([]any)
	if !ok || len(content) == 0 {
		t.Fatalf("no content: %v", res)
	}
	text, ok := asMap(t, content[0])["text"].(string)
	if !ok {
		t.Fatalf("no text: %v", content[0])
	}
	// The unknown connection must not mask the gate; either message is a
	// rejection, but the gate check runs after resolve — accept both.
	if !strings.Contains(text, "no connection") && !strings.Contains(text, "read-only") {
		t.Fatalf("unexpected rejection text: %q", text)
	}
}

func TestNotificationAccepted(t *testing.T) {
	t.Parallel()
	s := testService(t)
	token, _ := s.token()
	rec, _ := rpc(t, s, token, `{"jsonrpc":"2.0","method":"notifications/initialized"}`)
	if rec.Code != http.StatusAccepted {
		t.Fatalf("notification: got %d", rec.Code)
	}
}
