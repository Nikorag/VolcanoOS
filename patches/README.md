# Patches for other repos

## zonto-sdk-volcano-overlay.patch  (Nikorag/Zonto-Hub, `sdk/`)

Makes `@zonto/sdk` (TypeScript) and the Godot addon talk to the Volcano OS overlay:
first-time `unlock()` raises an achievement pop-up, plus `launcherNotify()` / `setLauncherStatus()` helpers.
Everything is a silent no-op unless the game was started by Volcano OS. Contract: [../docs/GAME-BRIDGE.md](../docs/GAME-BRIDGE.md).

Apply from the Zonto-Hub repo root:

    git apply --check /path/to/VolcanoOS/patches/zonto-sdk-volcano-overlay.patch
    git apply /path/to/VolcanoOS/patches/zonto-sdk-volcano-overlay.patch
    cd sdk && npm run build && npm run test:unit

Bumps `@zonto/sdk` to 0.8.0. Verified: applies cleanly to Zonto-Hub `main` (9a1368d); the 33 SDK unit tests pass
(7 new, in `sdk/test/launcher.test.mjs`); the GDScript parses under Godot 4.7. Not verified: the Godot code against a running
launcher, and `npm run test` (needs Playwright). `sdk/dist` is not part of the patch: rebuild it.
