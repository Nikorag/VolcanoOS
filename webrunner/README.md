# Web runner

Shows one Zonto web game in a bare window. The launcher starts it as a child process
(`--url=<game> --allow-origin=<zonto> --user-data-dir=<profile> [--windowed]`) and closes it
when the player leaves.

Why not just open a browser: browsers draw their own UI over the game ("update now" infobars,
permission prompts, an address bar) and can hand the page to an already-running instance.
This is Electron with all of that switched off:

- fullscreen kiosk window, no menu, no devtools, no infobars, nothing to update;
- permission requests are refused silently (fullscreen, pointer lock and keyboard lock are allowed);
- it can only ever navigate within Zonto's own origins, and refuses pop-up windows;
- a crash or a failed load exits non-zero, so the launcher sees the session end.

Files: `app/main.js` (the runner), `zonto-web-runner` (launch wrapper; falls back to `--no-sandbox`
only where the system forbids user namespaces), `bundle.sh` (assembles a pinned, checksum-verified
Electron plus the app into a directory; used by `packaging/appimage/build.sh`).

Testing: `zonto-web-runner ... --self-test` loads the page headlessly, checks the lockdowns
(navigation and `window.open` blocked, notification permission denied, devtools closed) plus
gamepad/fullscreen/WebGL availability, prints a `SELFTEST {...}` line and exits.

Dev overrides for the launcher: `VOS_WEBRUNNER=<path>` uses that runner, `VOS_BROWSER=<path>` uses
that browser instead, and `VOS_WEBVIEW=browser` forces this path on macOS (which uses WKWebView).
