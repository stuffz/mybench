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

Everything builds in containers — nothing installs on the host beyond docker and [task](https://taskfile.dev). The Taskfile is the only supported path: no ad-hoc `docker run`, `go test` or lint invocations, since they drift from the tasks (different linter versions give different results) and hide problems in the tasks themselves. Pass task vars as `VAR=value` (`task db:test PORT=4000`) or after `--`. A tool missing from the devcontainer goes into `.devcontainer/Dockerfile`, not around it. `task --list` shows the full surface; the core loop:

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

## Tests

`gui/tests/` mirrors `gui/src/` one directory deep. One executable and one CTest case per file, named `<layer>/<name>`, so `ctest -R editor` runs a layer and a crash takes down only that unit's process.

Each target links the unit under test plus its real dependency closure, spelled out in `gui/tests/CMakeLists.txt`. That list is not boilerplate: `tst_workspace` links 13 sources because `workspace.cpp` reaches into `EditorTab` through a `qobject_cast`. A new entry appearing there is a dependency you just added, and worth a second look.

`MYBENCH_BUILD_TESTS` is OFF by default, so the deb/arch/windows/macOS packaging builds configure exactly what they did before. `task gui:test` turns it on and compiles with `_GLIBCXX_ASSERTIONS`, which is what turns a `std::clamp` with crossed bounds from a formality into an abort. `task gui:coverage` reports per-file line coverage of `src/`.

`gui/tests/app/stubbackend.h` answers `/rpc` over loopback with just enough HTTP for `QNetworkAccessManager`. It is what lets the RPC client, the result model, the panels and the dialogs be driven with no backend process and no database. The seam is `Api::setEndpoint`; no production code exists to support the tests.

Rules that cost something to learn:

- Mutation-check every new suite: revert the behaviour in a scratch copy and confirm the test fails. A suite that passes either way is measuring nothing, and two of ours did.
- Never write a test that passes against a known bug. Fix the bug, or record it and leave the path untested.
- Check gate exit codes. `task … | grep | tail` returns `tail`'s status and will report a failing lint as clean.
- No `QDialog::exec()` under offscreen — it blocks forever. Drive widgets with `QCoreApplication::sendEvent` and click buttons directly. Better, arm a zero-timer that closes any modal that appears, so a regression fails instead of hanging.
- Drive timers with `QMetaObject::invokeMethod(timer, "timeout")`, never `QTest::qWait` on a poll interval.
- Find widgets by object name. `findChild<T *>(QString())` matches the *first* child of that type and silently follows constructor order, so adding a widget can redirect an unrelated test.
- Assert relationships and bounds, not pixels, fonts or wall-clock values. Where a pixel does matter, measure it (`QFontMetrics` against the widget's real width) rather than hardcoding one.

## Dependencies

Pin and declare only what is needed; prefer the standard library and existing dependencies. A new dependency needs a reason in the commit body (SQLite was declined in favour of the JSON workspace store).

## UI text

Title Case for named things ("Schema Graph", "Client Connections"), sentence case for descriptions. In dense chrome (status bar, kill buttons) prefer an icon with an instant tooltip over text.

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

Nothing identifying goes into writing (commit messages, release notes, docs, comments, issue text): no client or customer names, no production hostnames, no exact figures measured on a real system (uptime, row or query counts, byte volumes). Generalise ("a live server", "hundreds of thousands of queries a day"). Figures from the repo's own test setup are fine. Every binary stamps the commit SHA, so a leak in a pushed commit costs a history rewrite, a moved tag and a rebuild of every release artifact.
