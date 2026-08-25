# Release

One commit per release — version bumps ride in the change commit, never a separate chore commit. Release version is `<pkgver>-<pkgrel>`.

The app is the Qt GUI plus its Go backend — a sidecar process on Linux/macOS, linked into the exe on Windows; there is no web build any more. Release artifacts: the Windows exe, the Ubuntu .deb, the Arch package, and the macOS .app zip (`task gui:build:macos` on a Mac). The mac bundle is ad-hoc signed — no paid Apple identity — so the release notes must carry the quarantine-clearing instruction (`xattr -dr com.apple.quarantine`) beside the build-from-source option.

## 1. Bump versions

- `build/windows/info.json`: `file_version`/`ProductVersion` = pkgver.pkgrel (e.g. `0.1.0.9`) — it and icon.ico feed the GUI's app.rc (icon + the version metadata Explorer shows), and the same value is stamped into the linked-in backend (`buildinfo.Version`, the About dialog). The Linux tasks derive their versions from this file.
- `packaging/arch/PKGBUILD`: `pkgver`/`pkgrel` — the PKGBUILD is standalone (AUR-shaped), so it carries its own copy.

## 2. Commit

Everything in the one release commit — committing BEFORE building keeps the `git describe --dirty` build stamp clean.

## 3. Build

    task gui:build:windows
    task gui:build:deb        # needs task gui:image:deb once
    task gui:build:arch       # needs task gui:image:arch once; builds committed HEAD

- `bin/mybench.exe` — self-contained: static Qt with the Go backend linked in, nothing ships beside it.
- `bin/mybench_<ver>_amd64.deb` — built against Ubuntu 24.04 (Qt 6.4/glibc 2.39, the support floor), depends on the distro Qt. `task gui:test:deb` installs it in a clean ubuntu:24.04 and launches it under Xvfb (screenshot in `bin/deb-smoke.png`).
- `bin/mybench-<ver>-x86_64.pkg.tar.zst` — built from the committed HEAD in an archlinux container; depends on `qt6-base`/`qt6-svg`.

## 4. Tag and release

    git tag v<pkgver>-<pkgrel> && git push origin main v<pkgver>-<pkgrel>
    gh release create v<pkgver>-<pkgrel> --title "mybench <pkgver>-<pkgrel>" --notes "…" \
      bin/mybench.exe bin/mybench_*_amd64.deb bin/mybench-*-x86_64.pkg.tar.zst \
      bin/mybench-macos-*.zip

## 5. Install locally

    sudo pacman -U bin/mybench-<ver>-x86_64.pkg.tar.zst    # Arch host
    sudo apt install ./bin/mybench_<ver>_amd64.deb         # Ubuntu host
    task gui:install:macos                                 # Mac: /Applications/mybench.app
    task gui:install                                       # Linux without a package: ~/.local/bin
