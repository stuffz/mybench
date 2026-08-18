// mybench Electron shell: spawn the pure-Go server on an ephemeral loopback
// port and open a window at it. The entire app (bindings over HTTP) is the
// wails server mode that already runs in production — this file is only the
// window frame. No nodeIntegration, no preload: the page never touches Node.

const { app, BrowserWindow } = require("electron");
const { spawn } = require("node:child_process");
const net = require("node:net");
const http = require("node:http");
const path = require("node:path");
const fs = require("node:fs");

// Native Wayland when available (high-refresh compositors), X11 otherwise.
app.commandLine.appendSwitch("ozone-platform-hint", "auto");

// Wayland compositors pick the taskbar icon by matching the window's app_id
// to a desktop file — the BrowserWindow `icon` option is X11-only. This sets
// CHROME_DESKTOP so the app_id becomes "mybench", matching mybench.desktop.
app.setDesktopName("mybench.desktop");

let server = null;

// Dev layout: repo/desktop/main.js next to repo/bin + repo/build. Packaged
// layout (Arch package): everything installed flat next to main.js.
function appFile(name, ...devPath) {
  const candidates = [
    path.join(__dirname, name),
    path.join(__dirname, "..", ...devPath),
  ];
  return candidates.find((c) => fs.existsSync(c));
}

function serverBinary() {
  const bin = appFile("mybench-server", "bin", "mybench-server");
  if (!bin) throw new Error("mybench-server binary not found — build it: wails3 task build:server");
  return bin;
}

function freePort() {
  return new Promise((resolve, reject) => {
    const s = net.createServer();
    s.listen(0, "127.0.0.1", () => {
      const port = s.address().port;
      s.close(() => resolve(port));
    });
    s.on("error", reject);
  });
}

function waitReady(port, tries = 200) {
  return new Promise((resolve, reject) => {
    const attempt = (left) => {
      const req = http.get({ host: "127.0.0.1", port, path: "/" }, (res) => {
        res.resume();
        resolve();
      });
      req.on("error", () => {
        if (left <= 0) return reject(new Error("mybench-server did not become ready"));
        setTimeout(() => attempt(left - 1), 50);
      });
    };
    attempt(tries);
  });
}

// Window size persists across launches (~/.config/mybench/window-state.json).
// Position is saved too but Wayland compositors ignore it; size is honored.
const stateFile = () => path.join(app.getPath("userData"), "window-state.json");

function loadWindowState() {
  try {
    return JSON.parse(fs.readFileSync(stateFile(), "utf8"));
  } catch {
    return null;
  }
}

function saveWindowState(win) {
  const state = { ...win.getNormalBounds(), maximized: win.isMaximized() };
  try {
    fs.writeFileSync(stateFile(), JSON.stringify(state));
  } catch {
    // Not being able to persist geometry must never break quitting.
  }
}

async function start() {
  const port = await freePort();
  server = spawn(serverBinary(), [], {
    env: { ...process.env, WAILS_SERVER_HOST: "127.0.0.1", WAILS_SERVER_PORT: String(port) },
    stdio: "ignore",
  });
  server.on("exit", (code) => {
    // Server died out from under us — nothing sane to show.
    if (!app.isQuittingByUser && code !== 0 && code !== null) app.exit(1);
  });
  await waitReady(port);

  const st = loadWindowState();
  const win = new BrowserWindow({
    width: st?.width ?? 1280,
    height: st?.height ?? 800,
    x: st?.x,
    y: st?.y,
    backgroundColor: "#0a0a0c",
    autoHideMenuBar: true,
    title: "mybench",
    icon: appFile("appicon.png", "build", "appicon.png"),
  });
  if (st?.maximized) win.maximize();
  win.on("close", () => saveWindowState(win));
  await win.loadURL(`http://127.0.0.1:${port}/`);
}

app.whenReady().then(start).catch((err) => {
  console.error(err);
  app.exit(1);
});

app.on("window-all-closed", () => app.quit());
app.on("will-quit", () => {
  app.isQuittingByUser = true;
  if (server) server.kill();
});
