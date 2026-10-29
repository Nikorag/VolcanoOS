# Game <-> Volcano OS bridge

Games can put toasts, achievement pop-ups and a status line on the Volcano OS
overlay. Players open the overlay's menu with **Home** (Guide, or hold Start+Select).

## What the player sees
- **Toasts / achievements:** top-right, ~5.5s, max 3 on screen. Games are rate limited to a burst of 5 then 1 per second.
- **Menu (Home):** resume, switch to launcher / quit, session timer, your game's status line, friends online.
- **Invites:** the launcher polls `GET /api/invites?game=<slug>` every 30s while a game runs and shows "X invited you".
  It never accepts one: the game still does that itself through the API.

## Native games
The launcher starts a loopback-only HTTP server per session and passes:

| env var | value |
| --- | --- |
| `ZONTO_LAUNCHER_URL` | `http://127.0.0.1:<port>` |
| `ZONTO_LAUNCHER_TOKEN` | random bearer token, valid for this session only |
| `ZONTO_GAME_SLUG` | the game's slug |

If these are unset the game wasn't started by Volcano OS: do nothing.

```
POST $ZONTO_LAUNCHER_URL/v1/notify
Authorization: Bearer $ZONTO_LAUNCHER_TOKEN
{"kind":"achievement","title":"Forgemaster","body":"Forge a blade with three runes."}
```
- `kind`: `toast` (default) | `achievement` | `status`. Anything else is a 400.
- `title` <= 80 chars, `body` <= 200 chars. Text only; control characters are stripped.
- `POST /v1/status {"text":"Chapter 3"}` sets the status line shown in the menu.
- `GET /v1/ping` -> `{"ok":true,"launcher":"volcano-os","version":"..."}`.
- 401 without the token, 202 when accepted. Responses are fire-and-forget: never block gameplay on them.

## Web games
Volcano OS injects `window.ZontoLauncher` before the page loads (only in its own web renderer):
```js
if (window.ZontoLauncher) {
  ZontoLauncher.notify({ title: "Round 3", body: "Fight!" });
  ZontoLauncher.achievement("Forgemaster", "Forge a blade with three runes.");
  ZontoLauncher.setStatus("Chapter 3");
}
window.addEventListener("zontolauncher", () => { /* injected after the page started */ });
```

## Suggested `@zonto/sdk` additions (not applied: that repo is separate)
```ts
// sdk/src/launcher.ts
export function launcherNotify(ev: { kind?: "toast"|"achievement"|"status"; title?: string; body?: string }) {
  const w = globalThis as any;
  if (w.ZontoLauncher) return w.ZontoLauncher[ev.kind === "achievement" ? "achievement" : ev.kind === "status" ? "setStatus" : "notify"]?.(
    ...(ev.kind === "achievement" ? [ev.title, ev.body] : ev.kind === "status" ? [ev.body] : [ev]));
  const url = process.env.ZONTO_LAUNCHER_URL, token = process.env.ZONTO_LAUNCHER_TOKEN;  // native/Node hosts
  if (url && token) fetch(url + "/v1/notify", { method: "POST", headers: { Authorization: `Bearer ${token}` }, body: JSON.stringify(ev) }).catch(() => {});
}
// and in unlockAchievement(): after a successful API call, launcherNotify({ kind: "achievement", title: name, body: description })
```
Calling it from `unlockAchievement` gives every integrated game achievement pop-ups with no per-game work.

## Platform notes
- macOS: the overlay is a click-through, always-on-top window that never takes focus. It floats over windowed and
  borderless games and games in their own fullscreen Space. It may not appear over exclusive-fullscreen Metal/OpenGL apps.
- Elsewhere there is no native overlay window yet: the toasts and menu are drawn inside the launcher window.
- The game still receives controller input while the menu is open (the launcher can't capture it).
