# Release

One commit per release — version bumps ride in the change commit, never a
separate chore commit. Release version is `<pkgver>-<pkgrel>`.

## 1. Bump versions

- `build/arch/PKGBUILD`: `pkgrel` (or `pkgver` for a feature release). The
  deb takes its version from the PKGBUILD.
- `build/windows/info.json`: `file_version`/`ProductVersion` = pkgver.pkgrel
  (e.g. `0.1.0.9`), then regenerate the committed syso (devcontainer):
  `wails3 task windows:generate:syso ARCH=amd64`
- The mac `Info.plist` carries only the pkgver on purpose.

## 2. Commit

Everything in the one release commit — committing BEFORE building keeps the
`git describe --dirty` build stamp clean.

## 3. Build (devcontainer)

One task per invocation — `wails3 task` silently ignores extra task names:

    wails3 task build && wails3 task build:all && wails3 task windows:build

## 4. Package (host)

    go-task package:arch && go-task package:deb && go-task package:macos

## 5. Sign the mac app (any mac; ad-hoc — no Developer ID in play)

    scp -r bin/mybench.app laptop:/tmp/
    ssh laptop 'cd /tmp && codesign --force --deep --sign - mybench.app && \
      ditto -c -k --keepParent mybench.app mybench-macos-arm64.zip'
    scp laptop:/tmp/mybench-macos-arm64.zip bin/ && ssh laptop 'rm -rf /tmp/mybench.app /tmp/mybench-macos-arm64.zip'
    rm -rf bin/mybench.app bin/mybench

## 6. Tag and release

    git tag v<pkgver>-<pkgrel> && git push origin main v<pkgver>-<pkgrel>
    gh release create v<pkgver>-<pkgrel> --title "mybench <pkgver>-<pkgrel>" --notes "…" \
      bin/mybench-<pkgver>-<pkgrel>-x86_64.pkg.tar.zst bin/mybench_<pkgver>-<pkgrel>_amd64.deb \
      bin/mybench.exe bin/mybench-macos-arm64.zip

Only the four GUI apps go up — no server binaries.

## 7. Install locally

    go-task install                      # Arch host (sudo pacman -U)
    # laptop: unzip the release zip over /Applications/mybench.app
