# mybench

A small, fast MySQL GUI: run queries, browse schemas, and the two admin
panels Workbench does well (Users and Privileges, Client Connections) —
plus first-class Teleport tunnelling. Multiple servers open simultaneously
in one window.

Wails v3 backend (Go) with a React/TypeScript frontend. Every build is the
server-mode binary (`-tags server`); on Linux the desktop app is a thin
Electron shell (`desktop/`) that spawns that binary on a loopback port.

## Development

Backend/frontend work happens in the devcontainer (`.devcontainer/`):

```
wails3 task run:server    # dev server on :8080
wails3 task test          # go tests (MYBENCH_TEST_DSN set by the devcontainer)
wails3 task lint          # golangci-lint
```

Teleport testing needs the binary on the host (tsh session), not in the
container.

## Building

```
wails3 task build         # production server binary → bin/mybench-server
wails3 task build:all     # cross-compile linux-amd64, darwin-arm64, windows-amd64
wails3 task desktop       # run the Electron shell against bin/mybench-server (host)
wails3 task package:arch  # Arch package via makepkg (host)
wails3 task install       # package + sudo pacman -U (host)
```

## License

[MIT](LICENSE)
