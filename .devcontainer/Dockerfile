# Dev image: the Go toolchain lives here — nothing installs on the host.
# The GUI is built by its own images (gui/Dockerfile*); this container
# covers the backend: build, test, and lint.
FROM golang:trixie

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential pkg-config git ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

# Lint/format toolchain (rules in /work/backend/.golangci.yml)
RUN go install github.com/golangci/golangci-lint/v2/cmd/golangci-lint@latest

# Non-root user matching the host uid so files created on the bind mount stay
# owned by the host user. Cache dirs are created here so first-mount named
# volumes inherit dev's ownership.
RUN useradd -m -u 1000 dev && chown -R dev:dev /go
USER dev
ENV HOME=/home/dev
RUN mkdir -p /home/dev/.cache/go-build

WORKDIR /work
