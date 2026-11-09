# Apps

Apps are self-contained programs that live next to games on Home (the **Apps** tab, and at the end of **All**). They draw their own screens with the same toolkit as the rest of the launcher, keep their own account, and can hand the screen to an external player.

## The framework (`src/apps.hpp`)

An app derives from `AppModule` and is registered in `App::initApps()` (`src/app.cpp`).

| You implement | What it is for |
| --- | --- |
| `info()` | id, name, tagline, blurb, tile art (`file://` URL), accent colour |
| `open()` | the player opened it from Home |
| `handle(Action)` | controller input; return `true` to close the app (Back at the top level) |
| `draw(now, dt, style)` | one frame |
| `tick(now)` | once per frame while the app is on screen |
| `status()` | the "Account" line on Home's info card |
| `loadDemo()` / `demoScreen(name)` | fixtures for `--demo` and screenshots |
| `sessionAction / sessionTick / sessionEnded / sessionStatus` | while a player process runs (below) |

The `AppContext` it receives gives it the `Gfx`, `Art` (cached remote images), the worker `Pool` + `MainQueue`, toasts, the on-screen keyboard (`text`), a confirm dialog and `launch`.

### Playback sessions

`ctx.launch(AppLaunch{exe, args, env, title})` starts the player as a normal session. While it runs:

- The launcher keeps reading the controller and relays presses to `sessionAction`, so an app can drive its player (the player does not need gamepad support).
- `sessionTick` runs every frame; `sessionEnded(exitCode)` when the process exits.
- **Home** (Guide, or Start + Select) opens the Volcano OS menu over the player where an overlay window exists; elsewhere it stops playback and returns to the launcher.
- Apps are not games: no Zonto presence, no playtime, no game invites.

Apps appear on Home as `GameEntry`s with slug `app:<id>`; `App::primaryFor` turns that into an **Open** button.

## Jellyfin (`src/jellyfin*.{hpp,cpp}`, `src/mpv_ipc.*`)

- `Jellyfin` is the blocking REST client (run it on the Pool): LAN discovery (UDP 7359), probe, Quick Connect, password sign-in, views / resume / next up / latest / children / search / seasons / episodes, mark played, and playback reports.
- `JellyfinApp` is the UI: setup (server list or address, Quick Connect code, password), Home rows, library grid, show/movie/episode pages, search.
- `MpvIpc` talks to mpv's `--input-ipc-server` socket: it observes `time-pos`, `duration` and `pause`, and sends commands.

**Playback.** The launcher runs `mpv` fullscreen on the direct-play stream (`/Videos/<id>/stream?static=true`), with the access token in an `X-Emby-Token` header (never in the URL or `ps`), `--start=` for resume. Start, progress (every 10 s) and stop are reported to `/Sessions/Playing*`, so resume points and watched state follow you to other devices.

**Controller during playback:** A play/pause, ◀ ▶ seek 10 s, LB / RB seek 60 s, ▲ ▼ volume, X subtitles, Y audio track, Start/Select show progress, B stop.

**State.** `jellyfin.json` in the data directory (mode 0600) holds the device id and the session. Signing out forgets the token.

**mpv.** Found on `PATH` (plus the usual Homebrew/`/usr/bin` locations); set `VOS_MPV` to override. The SD-card image installs it.

### Not built yet

Transcoding fallback (everything is direct play, so a file the device cannot decode will not play), sidecar subtitle files, music and live TV, multiple users per server, and Windows remote control (`MpvIpc` is a no-op there; playback still starts).

### Testing without a server

```sh
python3 tools/mock-jellyfin.py --port 18096 --log /tmp/reports.log &   # user jamie / pw; Quick Connect approves on the 2nd poll
VOS_MPV=tools/fake-mpv.py FAKE_MPV_LOG=/tmp/mpv.log \
  ZontoVolcano --app jellyfin --windowed          # --app skips Zonto sign-in
```

`--demo --screen jellyfin | jellyfin-detail | jellyfin-movie | jellyfin-grid | jellyfin-setup | jellyfin-code | jellyfin-password` render fixture screens (they are part of `tools/ui-lint.sh`).
