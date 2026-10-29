# Zonto Volcano

**Volcano OS** is the software for the **Zonto Volcano** console: a controller-first "Big Picture" launcher for [Zonto Games](https://www.nikorag.co.uk), written in C++17 with SDL2.

- **Sign in** via OAuth 2.0 + PKCE (loopback redirect, `zonto-launcher` client): your real browser does the login, token goes in the macOS Keychain (0600 file elsewhere).
- **Library** from `GET /api/library?platform=&arch=`, cached for offline use.
- **Install / update / uninstall** native builds: resumable download with a re-signed URL per attempt, SHA-256 verify, libarchive extraction (zip + tar.gz, path-traversal guarded), atomic promote, per-device install sync (`PUT /api/devices`).
- **Launch** native games as supervised processes (`.app` via `open -W`), with presence heartbeats and wall-clock playtime reporting when the game doesn't self-report.
- **Web games only** open in a separate renderer (WKWebView on macOS; a Chromium app-mode window elsewhere) using the ticketed URL from `POST /api/library/{slug}/launch`. The web view can't load anything else.
- **Native pages, no web content:** Achievements, Social (friends, requests, activity, chat), and Profile (stats, privacy settings, devices) are drawn with SDL from the JSON APIs, with an on-screen keyboard for typing. The Store isn't built yet: it needs new endpoints, see [docs/API-GAPS.md](docs/API-GAPS.md).
- **Controller first**: Xbox / PlayStation / Nintendo glyphs, stick + d-pad repeat, haptic ticks, keyboard fallback. `Start+Select` (or Guide) = Home: leaves a web game, or offers to quit a native one.

- **In-game overlay:** Home (Guide, or hold Start+Select) opens a Volcano OS menu over a running game, and games can send toasts / achievement pop-ups through a loopback bridge. See [docs/GAME-BRIDGE.md](docs/GAME-BRIDGE.md).

## Build (macOS)
    brew install sdl2 sdl2_ttf sdl2_image libarchive cmake
    cmake -S . -B build && cmake --build build -j
    ./build/ZontoVolcano            # fullscreen; F11 toggles
    ./build/ZontoVolcano --demo --windowed   # offline fixture library
    ZONTO_API=http://localhost:3100 ./build/ZontoVolcano   # local server

Options: `--shot out.png --press right,right,a` renders a scripted demo frame to PNG. See `--help`.
Fonts: JetBrains Mono and Archivo (OFL) in `assets/fonts`.

## Console image (Raspberry Pi)
`./image/build.sh` builds an SD-card image: stripped Raspberry Pi OS Lite that boots straight into Volcano OS, with controller support, networking, and native **Wi-Fi** and **Bluetooth** pages in the launcher (Start -> Network / Bluetooth). See [image/README.md](image/README.md).

## Status
Developed and tested on macOS. Linux/Windows code paths (sockets, process spawn, Chromium fallback) are written but untested.
