'use strict';
// Volcano OS web runner. Shows one Zonto web game in a bare window: no address bar, no
// infobars, no "update now" or permission popups, no devtools, and it can only ever show
// Zonto. It is started (and closed) by the launcher as a child process.
//
//   zonto-web-runner --url=<https://...> [--allow-origin=<origin>] [--user-data-dir=<dir>]
//                    [--windowed] [--self-test]
const { app, BrowserWindow, Menu } = require('electron');

const arg = (name) => {
  const p = `--${name}=`;
  const a = process.argv.find((x) => x.startsWith(p));
  return a ? a.slice(p.length) : null;
};
const flag = (name) => process.argv.includes(`--${name}`);

const startUrl = arg('url');
const windowed = flag('windowed');
const selfTest = flag('self-test');

function originOf(u) {
  try {
    const url = new URL(u);
    return url.protocol === 'https:' || url.protocol === 'http:' ? url.origin : null;
  } catch {
    return null;
  }
}

// Only Zonto: the game's own origin plus any the launcher names (its API origin, for redirects).
const allowedOrigins = new Set();
for (const u of [startUrl, ...(arg('allow-origin') || '').split(',')]) {
  const o = u && originOf(u);
  if (o) allowedOrigins.add(o);
}
const isAllowed = (u) => {
  const o = originOf(u);
  return o !== null && allowedOrigins.has(o);
};

if (!startUrl || !isAllowed(startUrl)) {
  console.error('zonto-web-runner: --url must be an http(s) URL');
  app.exit(2);
}

// Where the profile lives. Never the player's own browser profile.
if (arg('user-data-dir')) app.setPath('userData', arg('user-data-dir'));

// A console has no browser UI to explain itself: nothing may ask the player anything.
app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required');
app.commandLine.appendSwitch('ozone-platform-hint', 'auto'); // Wayland where there is one, else X11
if (process.env.VOS_WEB_NOSANDBOX === '1') app.commandLine.appendSwitch('no-sandbox');

// Never leave a window with nothing behind it: if we die, the launcher sees the session end.
process.on('SIGTERM', () => app.exit(0));
process.on('SIGINT', () => app.exit(0));

const PERMISSIONS_ALLOWED = new Set(['fullscreen', 'pointerLock', 'keyboardLock', 'clipboard-sanitized-write']);

function createWindow() {
  Menu.setApplicationMenu(null);

  const win = new BrowserWindow({
    show: false,
    fullscreen: !windowed,
    kiosk: !windowed,
    frame: windowed,
    width: 1280,
    height: 800,
    backgroundColor: '#0b0908',
    autoHideMenuBar: true,
    title: 'Zonto',
    webPreferences: {
      contextIsolation: true,
      sandbox: true,
      nodeIntegration: false,
      devTools: false,
      spellcheck: false,
      backgroundThrottling: false, // a game must keep running when the launcher has focus
      autoplayPolicy: 'no-user-gesture-required',
    },
  });

  const wc = win.webContents;
  const ses = wc.session;

  // No permission popups: allow what a game needs to be a game, refuse (silently) the rest.
  ses.setPermissionRequestHandler((_wc, permission, cb) => cb(PERMISSIONS_ALLOWED.has(permission)));
  ses.setPermissionCheckHandler((_wc, permission) => PERMISSIONS_ALLOWED.has(permission));

  // Stay on Zonto: no navigating away, no pop-up windows.
  const guard = (e, u) => {
    if (!isAllowed(u)) e.preventDefault();
  };
  wc.on('will-navigate', guard);
  wc.on('will-redirect', guard);
  wc.setWindowOpenHandler(() => ({ action: 'deny' }));

  // No "Leave site?" prompts when the launcher closes us.
  wc.on('will-prevent-unload', (e) => e.preventDefault());

  // Fail visibly to the launcher rather than leaving a blank window up.
  wc.on('render-process-gone', (_e, details) => {
    console.error(`renderer gone: ${details.reason}`);
    app.exit(1);
  });
  wc.on('did-fail-load', (_e, code, desc, failedUrl, isMainFrame) => {
    if (!isMainFrame || code === -3) return; // -3 = aborted (a redirect), harmless
    console.error(`load failed: ${desc} (${code}) ${failedUrl}`);
    if (!selfTest) setTimeout(() => app.exit(3), 1500);
  });

  win.on('closed', () => app.quit());

  if (selfTest) {
    wc.once('did-finish-load', () => runSelfTest(win));
  } else {
    win.once('ready-to-show', () => win.show());
  }
  win.loadURL(startUrl);
}

// Headless check used by tests: the lockdowns above actually hold in a real Electron.
async function runSelfTest(win) {
  const wc = win.webContents;
  const before = wc.getURL();
  const result = { title: wc.getTitle(), url: before };
  const run = (code) => wc.executeJavaScript(code, true);
  try {
    result.notifications = await run('Notification.requestPermission()'); // popup-free: must be "denied"
    result.windowOpenBlocked = (await run("window.open('http://example.invalid/') === null")) === true;
    await run("location.href = 'http://blocked.invalid/'; 0");
    await new Promise((r) => setTimeout(r, 600));
    result.navigationBlocked = wc.getURL() === before;
    result.gamepadApi = await run("typeof navigator.getGamepads === 'function'");
    result.fullscreenApi = await run("typeof document.documentElement.requestFullscreen === 'function'");
    result.webgl = await run("!!document.createElement('canvas').getContext('webgl')");
    result.devTools = wc.isDevToolsOpened();
  } catch (err) {
    result.error = String(err);
  }
  console.log('SELFTEST ' + JSON.stringify(result));
  app.exit(result.error ? 1 : 0);
}

app.whenReady().then(createWindow);
app.on('window-all-closed', () => app.quit());
