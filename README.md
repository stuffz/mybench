# mybench

A small, fast MySQL GUI: run queries, browse schemas, watch a live server dashboard, and the admin panels Workbench does well (Users and Privileges, Client Connections) — plus first-class Teleport tunnelling with quick connect. Multiple servers open simultaneously in one window.

Two trees, one app: `gui/` is a native Qt 6 client, `backend/` is the Go engine it spawns as a sidecar and drives over JSON-RPC (connections, SSH and Teleport tunnels, keyring, query sessions, history, the MCP endpoint).

## Development

Everything builds in containers — nothing installs on the host. `task` from the repo root:

```
task gui:image          # Qt6 build image (once)
task gui:build          # backend + GUI → gui/build/mybench
task gui:run            # build and run on the host display
task db:test            # seeded MySQL 8.4 on 127.0.0.1:3307 (root/devroot, PORT=… to move)
task test               # go tests (MYBENCH_TEST_DSN set by the devcontainer)
task lint               # golangci-lint (backend) + clang-tidy (gui)
task format             # gofumpt (backend) + clang-format (gui) rewrite
task check              # full gate — tests, lint and format check
```

Teleport testing needs the binaries on the host (tsh session), not in a container.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the house style and workflow.

## Building

```
task gui:image:windows  # static Qt6 cross toolchain image (once, ~5 min)
task gui:build:windows  # self-contained bin/mybench.exe (backend linked in)
task gui:build:deb      # Ubuntu 24.04+ package (image via gui:image:deb once; gui:test:deb smoke-tests it)
task gui:build:arch     # Arch package (image via gui:image:arch once; builds the committed HEAD)
task gui:build:macos    # ad-hoc-signed .app zipped into bin/ — run on a Mac
task gui:install        # Linux without a package: stripped binaries into ~/.local/bin
task gui:install:macos  # macOS: build and install /Applications/mybench.app
```

Releases follow [RELEASE.md](RELEASE.md).

## License

[MIT](LICENSE)
