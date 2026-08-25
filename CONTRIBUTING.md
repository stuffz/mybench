# Contributing

House rules and workflow for mybench. The formatter and linters enforce what they can; this file covers the rest — so the codebase keeps reading like one author. Everything here applies to agents as much as humans.

## Layout

Two trees, one app: `gui/` is the native Qt 6 client, `backend/` is the Go engine it spawns as a sidecar and drives over JSON-RPC (connections, SSH and Teleport tunnels, keyring, query sessions, history, the MCP endpoint). `build/` holds the icons and the Windows version metadata the GUI embeds; `packaging/` holds the Arch PKGBUILD, the deb control template and the shared .desktop file; `bin/` is build output only.

`gui/src/` is grouped by layer:

- `app/` — process glue: backend spawn, RPC client, theme, icons
- `shell/` — window chrome and workspace persistence: mainwindow, server tabs, status strip, sidebar
- `dialogs/` — modal dialogs
- `editor/` — the query tab: SQL editor, highlighting, results grid and pages, plan view
- `views/` — admin panels and inspectors
- `ui/` — shared widget helpers, value formatters, table utilities

One class per file. A file-local helper lives in an anonymous namespace next to its only user; the moment a second file needs it, it moves to `ui/` with a declaration in a header — never duplicated.

## Development

Everything builds in containers — nothing installs on the host beyond docker and [task](https://taskfile.dev). `task --list` shows the full surface; the core loop:

```
task gui:image          # Qt6 build image (once)
task gui:build          # backend + GUI → gui/build/mybench
task gui:run            # build and run on the host display
task db:test            # seeded MySQL 8.4 on 127.0.0.1:3307 (root/devroot, PORT=… to move)
task gui:image:windows  # static Qt6 cross toolchain image (once, ~5 min)
task gui:build:windows  # self-contained bin/mybench.exe (backend linked in)
```

The backend also has a devcontainer (`.devcontainer/`) that provides the Go toolchain, golangci-lint, and the test databases with `MYBENCH_TEST_DSN` set.

Teleport testing needs the binaries on the host (tsh session), not in a container.

## Gates

Before a change is done:

```
task format             # gofumpt + clang-format rewrite
task check              # tests + lint + format-check in one shot
task gui:build:windows  # must link bin/mybench.exe
```

Format before lint; lint before calling anything finished. `gui:lint` builds a dedicated tree on container-local disk first — on a Windows checkout the bind mount can't take CMake's configure step, so don't "simplify" that away.

## C++ style (gui/)

### Formatting

`gui/.clang-format` is law: Allman braces, 4-space indent, 100 columns, `Type *p`, BlockIndent continuations (wrapped calls break after `(` and continue at +4 — no paren-alignment), and `InsertBraces` puts braces on every control-flow body, single statements included. Never hand-fight the formatter; write plainly and run `task gui:format`.

Includes are dir-qualified (`#include "app/theme.h"`), project includes first, then Qt, separated by a blank line; the file's own header stands alone at the top. The formatter sorts within each block (`IncludeBlocks: Preserve` keeps the groups apart). clang-tidy's `misc-include-cleaner` requires a direct include for every symbol's providing header — transitive includes don't count.

New headers: `#pragma once` plus a one-or-two-line comment saying what the file owns (see `shell/mainwindow.h` for the tone).

### Comments

Explain constraints the code can't show — the "why", a Qt quirk, an ordering requirement. Never narrate the next line, and never leave provenance notes ("moved from X", "extracted from Y"): a file must read as if it were born where it is.

### Builder functions (the PanelBase pattern)

Widget-building code is grouped, not streamed. `views/panelbase.cpp` (PanelBase's constructor) is the canonical shape:

- Blocks separated by blank lines; a `// --- name ------` or short intent comment opens a block when it isn't obvious.
- Create-then-assemble: construct and configure each widget in its own stanza, then do the `addWidget` calls together at the end of the block.
- Intermediate names say what a thing means (`QFont medium`, not `f`).

### Qt traps to respect

- Widget text is never sized with `QWidget::setFont` — the app stylesheet's global font rule silently overrides it. Size through the sheet (objectNames like `#smallText`, `#kpiValue`) or `theme::scaledPx`; the full warning lives in `app/theme.h`.
- Stylesheet borders on plain-QWidget subclasses need `Qt::WA_StyledBackground` or they don't paint.
- Construction order inside builders can be load-bearing (Qt parent/child relationships); preserve it when moving code.
- Sizes are px, derived from the theme's DPI-corrected base font — no hardcoded px for anything that should scale with the font slider or monitor scale.

## Go style (backend/)

`backend/.golangci.yml` is the linter contract; `task lint` runs it and `task test` runs the tests (the devcontainer provides the DSN). Services expose methods to the GUI over JSON-RPC — exported methods are the API surface, so renaming one is a protocol change. The workspace blob (`shell/workspace.{h,cpp}` on the GUI side) is a stable format: key names are frozen at version 2.

## Commits

Conventional-commit style, present tense, with the subsystem as scope: `fix(gui): …`, `refactor(backend): …`, `style(gui): …`. The body explains why and what a reader needs to trust the change — measurements, invariants preserved, what was deliberately not done. Mechanical changes (reformats, moves) stay in their own commits, separate from behavior changes. Releases follow [RELEASE.md](RELEASE.md).
