package conn

// Teleport helpers exposed to the frontend: login status for the connection
// dialog and the database list for the resource picker. Both are read-only
// tsh commands — nothing here touches a database.

import (
	"context"
	"encoding/json"
	"fmt"
	"os/exec"
	"strings"
	"time"
)

const tshTimeout = 15 * time.Second

// TeleportStatus reports whether tsh exists and has a valid session, plus
// the session facts the dialog renders (parsed from `tsh status -f json`).
type TeleportStatus struct {
	Found      bool     `json:"found"`
	LoggedIn   bool     `json:"loggedIn"`
	Detail     string   `json:"detail"` // help/error text when not usable
	Username   string   `json:"username"`
	Cluster    string   `json:"cluster"`
	Roles      []string `json:"roles"`
	DBUsers    []string `json:"dbUsers"`   // traits: db users the certs allow
	Databases  []string `json:"databases"` // db resources with active certs
	ValidUntil string   `json:"validUntil"` // RFC3339; empty if unknown
}

// TeleportDB is one database resource from `tsh db ls`.
type TeleportDB struct {
	Name        string   `json:"name"`
	Protocol    string   `json:"protocol"`
	Description string   `json:"description"`
	Users       []string `json:"users"` // allowed db users ("*" possible)
}

// TeleportStatus checks the tsh session so the dialog can say "logged in as …"
// or "run tsh login" instead of failing later inside the tunnel.
func (s *Service) TeleportStatus() *TeleportStatus {
	if _, err := exec.LookPath("tsh"); err != nil {
		return &TeleportStatus{Detail: "tsh not found on PATH — install the Teleport client"}
	}
	ctx, cancel := context.WithTimeout(context.Background(), tshTimeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, "tsh", "status", "-f", "json")
	var stderr strings.Builder
	cmd.Stderr = &stderr
	out, err := cmd.Output()
	if err != nil {
		detail := strings.TrimSpace(stderr.String())
		if detail == "" {
			detail = err.Error()
		}
		return &TeleportStatus{Found: true, Detail: "not logged in — run `tsh login`: " + detail}
	}
	var raw struct {
		Active struct {
			Username string   `json:"username"`
			Cluster  string   `json:"cluster"`
			Roles    []string `json:"roles"`
			Traits   struct {
				DBUsers []string `json:"db_users"`
			} `json:"traits"`
			Databases  []string  `json:"databases"`
			ValidUntil time.Time `json:"valid_until"`
		} `json:"active"`
	}
	if err := json.Unmarshal(out, &raw); err != nil {
		return &TeleportStatus{Found: true, Detail: "parse tsh status output: " + err.Error()}
	}
	a := raw.Active
	if a.Username == "" {
		return &TeleportStatus{Found: true, Detail: "not logged in — run `tsh login`"}
	}
	st := &TeleportStatus{
		Found:     true,
		LoggedIn:  true,
		Username:  a.Username,
		Cluster:   a.Cluster,
		Roles:     a.Roles,
		DBUsers:   a.Traits.DBUsers,
		Databases: a.Databases,
	}
	if !a.ValidUntil.IsZero() {
		st.ValidUntil = a.ValidUntil.Format(time.RFC3339)
		if time.Now().After(a.ValidUntil) {
			st.LoggedIn = false
			st.Detail = "session expired — run `tsh login`"
		}
	}
	return st
}

// TeleportDatabases lists database resources for the picker (read-only).
func (s *Service) TeleportDatabases() ([]TeleportDB, error) {
	if _, err := exec.LookPath("tsh"); err != nil {
		return nil, fmt.Errorf("tsh not found on PATH")
	}
	ctx, cancel := context.WithTimeout(context.Background(), tshTimeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, "tsh", "db", "ls", "--format=json")
	var stderr strings.Builder
	cmd.Stderr = &stderr
	out, err := cmd.Output()
	if err != nil {
		msg := strings.TrimSpace(stderr.String())
		if msg == "" {
			msg = err.Error()
		}
		return nil, fmt.Errorf("tsh db ls: %s", msg)
	}
	var raw []struct {
		Metadata struct {
			Name        string `json:"name"`
			Description string `json:"description"`
		} `json:"metadata"`
		Spec struct {
			Protocol string `json:"protocol"`
		} `json:"spec"`
		Users struct {
			Allowed []string `json:"allowed"`
		} `json:"users"`
	}
	if err := json.Unmarshal(out, &raw); err != nil {
		return nil, fmt.Errorf("parse tsh db ls output: %w", err)
	}
	dbs := make([]TeleportDB, 0, len(raw))
	for _, r := range raw {
		dbs = append(dbs, TeleportDB{
			Name:        r.Metadata.Name,
			Protocol:    r.Spec.Protocol,
			Description: r.Metadata.Description,
			Users:       r.Users.Allowed,
		})
	}
	return dbs, nil
}
