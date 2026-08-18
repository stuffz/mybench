# Dev image: the entire toolchain lives here — nothing installs on the host.
# golang:trixie because current Wails v3 on Linux links GTK4 + WebKitGTK 6.0
# (older docs say gtk3/webkit2gtk-4.1 — stale), which Debian 13 (trixie)
# carries. Pin the digest once the first build verifies.
FROM golang:trixie

ENV DEBIAN_FRONTEND=noninteractive

# Wails v3 Linux deps + headless window stub + basics
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential pkg-config git ca-certificates curl \
        libgtk-4-dev libwebkitgtk-6.0-dev \
        xvfb xauth dbus \
    && rm -rf /var/lib/apt/lists/*

# Node: major version from .nvmrc (build arg keeps the pin in one place)
ARG NODE_MAJOR=24
RUN curl -fsSL https://deb.nodesource.com/setup_${NODE_MAJOR}.x | bash - \
    && apt-get install -y nodejs \
    && rm -rf /var/lib/apt/lists/*

# pnpm via corepack — package manager with lifecycle scripts blocked by default
RUN corepack enable && corepack prepare pnpm@latest --activate

# Wails v3 CLI + task runner it drives
RUN go install github.com/wailsapp/wails/v3/cmd/wails3@latest

# Lint/format toolchain (rules in /work/.golangci.yml)
RUN go install github.com/golangci/golangci-lint/v2/cmd/golangci-lint@latest

# Non-root user matching the host uid so files created on the bind mount stay
# owned by the host user. Cache dirs are created here so first-mount named
# volumes inherit dev's ownership.
RUN useradd -m -u 1000 dev && chown -R dev:dev /go
USER dev
ENV HOME=/home/dev
RUN mkdir -p /home/dev/.cache/go-build /home/dev/.local/share/pnpm/store

WORKDIR /work
