package workspace

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/stuffz/mybench/internal/storage"
)

func newService(t *testing.T) (*Service, *storage.Store) {
	t.Helper()
	store, err := storage.Open(filepath.Join(t.TempDir(), "test.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })

	// New() points legacyPath at the real user config dir. Left alone, a
	// developer's own workspace.json would leak into the test.
	svc := New(store)
	svc.legacyPath = filepath.Join(t.TempDir(), "workspace.json")
	return svc, store
}

func TestLoadOnFirstLaunch(t *testing.T) {
	t.Parallel()
	svc, _ := newService(t)

	blob, err := svc.Load()
	if err != nil || blob != "" {
		t.Fatalf("fresh install: blob=%q err=%v", blob, err)
	}
}

func TestSaveThenLoad(t *testing.T) {
	t.Parallel()
	svc, _ := newService(t)

	if err := svc.Save(`{"version":2}`); err != nil {
		t.Fatal(err)
	}
	blob, err := svc.Load()
	if err != nil || blob != `{"version":2}` {
		t.Fatalf("got %q err=%v", blob, err)
	}
}

// The blob is opaque to the backend: it gives the frontend's JSON a durable
// home and must hand back exactly what it was given, byte for byte.
func TestSaveIsOpaque(t *testing.T) {
	t.Parallel()
	svc, _ := newService(t)

	const blob = "not json at all \x00\xef\xbb\xbf {\"trailing\": "
	if err := svc.Save(blob); err != nil {
		t.Fatal(err)
	}
	got, err := svc.Load()
	if err != nil {
		t.Fatal(err)
	}
	if got != blob {
		t.Fatalf("blob changed in transit:\n got %q\nwant %q", got, blob)
	}
}

func TestLegacyFileIsImportedOnce(t *testing.T) {
	t.Parallel()
	svc, store := newService(t)
	if err := os.WriteFile(svc.legacyPath, []byte(`{"from":"legacy"}`), 0o600); err != nil {
		t.Fatal(err)
	}

	blob, err := svc.Load()
	if err != nil || blob != `{"from":"legacy"}` {
		t.Fatalf("first load: blob=%q err=%v", blob, err)
	}

	// Imported, not merely read: the next launch must not depend on the file
	// still being there.
	stored, err := store.LoadWorkspace()
	if err != nil || stored != `{"from":"legacy"}` {
		t.Fatalf("not written through: stored=%q err=%v", stored, err)
	}

	// The file stays as a backup, but is never read again, so edits to it do
	// not resurrect an old session over the live one.
	if err := os.WriteFile(svc.legacyPath, []byte(`{"from":"edited"}`), 0o600); err != nil {
		t.Fatal(err)
	}
	if blob, err = svc.Load(); err != nil || blob != `{"from":"legacy"}` {
		t.Fatalf("second load re-read the legacy file: blob=%q err=%v", blob, err)
	}
}

func TestStoredBlobWinsOverLegacyFile(t *testing.T) {
	t.Parallel()
	svc, _ := newService(t)
	if err := os.WriteFile(svc.legacyPath, []byte(`{"from":"legacy"}`), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := svc.Save(`{"from":"store"}`); err != nil {
		t.Fatal(err)
	}

	blob, err := svc.Load()
	if err != nil || blob != `{"from":"store"}` {
		t.Fatalf("legacy shadowed the live session: blob=%q err=%v", blob, err)
	}
}

// A legacy path that exists but cannot be read is not "first launch": treating
// it as one would silently hand back an empty session and then overwrite the
// stored blob with it.
func TestUnreadableLegacyFileIsAnError(t *testing.T) {
	t.Parallel()
	svc, _ := newService(t)
	if err := os.Mkdir(svc.legacyPath, 0o700); err != nil {
		t.Fatal(err)
	}

	blob, err := svc.Load()
	if err == nil {
		t.Fatalf("want an error, got blob=%q", blob)
	}
	if blob != "" {
		t.Fatalf("want no blob alongside the error, got %q", blob)
	}
}
