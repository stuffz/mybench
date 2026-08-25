#!/bin/sh
# Clean-room test for the built .deb: a stock ubuntu:24.04 container (repo
# mounted at /work) installs the newest bin/mybench_*.deb — which proves the
# generated Depends line — then launches the app under Xvfb and screenshots
# it to bin/deb-smoke.png. Run via `task gui:test:deb`.
set -eu

apt-get update -q
deb=$(ls -t /work/bin/mybench_*_amd64.deb | head -1)
apt-get install -yq --no-install-recommends \
    "$deb" xvfb xdotool imagemagick fonts-dejavu-core

xvfb-run -a sh -euc '
    mybench &
    pid=$!
    sleep 8
    # Still running after 8s: the backend sidecar spawned and the window is up
    # (a backend failure pops a fatal dialog; the process staying alive plus
    # the window check below is the pass signal).
    kill -0 "$pid"
    xdotool search --name mybench
    import -window root /work/bin/deb-smoke.png
    kill "$pid"
'
echo "deb smoke test passed - screenshot in bin/deb-smoke.png"
