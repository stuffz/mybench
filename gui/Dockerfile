# Qt6 build image for the GUI. Debian trixie matches the devcontainer
# base, and carries Qt 6.8 — Widgets, Gui, Network are all we link.
# Nothing installs on the host: cmake/ninja/g++/Qt all live here.
FROM debian:trixie

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config \
        qt6-base-dev qt6-base-dev-tools qt6-svg-dev \
        libgl1 libegl1 libxkbcommon-x11-0 \
        xvfb xdotool imagemagick curl ca-certificates \
        clang-format clang-tidy gcovr \
    && rm -rf /var/lib/apt/lists/*

# Match the host uid so build artifacts on the bind mount stay writable.
RUN useradd -m -u 1000 dev
USER dev
WORKDIR /work/gui
