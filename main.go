package main

import (
	"embed"
	"log"
	"os"
	"runtime"
	"strings"

	"github.com/wailsapp/wails/v3/pkg/application"

	"github.com/stuffz/mybench/internal/admin"
	"github.com/stuffz/mybench/internal/conn"
	"github.com/stuffz/mybench/internal/mcp"
	"github.com/stuffz/mybench/internal/query"
	"github.com/stuffz/mybench/internal/storage"
	"github.com/stuffz/mybench/internal/workspace"
)

//go:embed all:frontend/dist
var assets embed.FS

const appName = "mybench"

// Finder/Spotlight launches on macOS inherit launchd's minimal PATH, not the
// login shell's, so tools resolved via exec.LookPath (tsh) are invisible.
// Append the standard Homebrew/local dirs when missing.
func fixDarwinPath() {
	path := os.Getenv("PATH")
	for _, dir := range []string{"/opt/homebrew/bin", "/usr/local/bin"} {
		if !strings.Contains(":"+path+":", ":"+dir+":") {
			path += ":" + dir
		}
	}
	os.Setenv("PATH", path)
}

func main() {
	if runtime.GOOS == "darwin" {
		fixDarwinPath()
	}
	conns := conn.New()
	store, err := storage.Open(storage.DefaultPath())
	if err != nil {
		log.Fatalf("open storage: %v", err)
	}
	adminSvc := admin.New(conns)
	app := application.New(application.Options{
		Name:        appName,
		Description: "Fast MySQL GUI",
		Services: []application.Service{
			application.NewService(conns),
			application.NewService(query.New(conns, store)),
			application.NewService(adminSvc),
			application.NewService(workspace.New(store)),
			application.NewService(mcp.New(conns, adminSvc, store)),
		},
		Assets: application.AssetOptions{
			Handler: application.AssetFileServerFS(assets),
		},
		Mac: application.MacOptions{
			ApplicationShouldTerminateAfterLastWindowClosed: true,
		},
	})

	app.Window.NewWithOptions(application.WebviewWindowOptions{
		Title:            appName,
		Width:            1280,
		Height:           800,
		BackgroundColour: application.NewRGB(10, 10, 12),
		URL:              "/",
	})

	if err := app.Run(); err != nil {
		log.Fatal(err)
	}
}
