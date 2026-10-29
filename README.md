# Zonto Volcano

**Volcano OS** is the software for the **Zonto Volcano** console: a controller-first "Big Picture" launcher for [Zonto Games](https://www.nikorag.co.uk), written in C++17 with SDL2.

- **Sign in on your phone:** the launcher shows a code and a QR (OAuth 2.0 device authorization grant, RFC 8628, `zonto-launcher` client); approve it on your phone and the console is signed in, no keyboard needed. Access tokens last an hour and refresh automatically with a rotating refresh token, so a console used at least every two months stays signed in. The browser flow (authorization code + PKCE over a loopback redirect) remains as a fallback (`Y` on the sign-in screen). Credentials go in the macOS Keychain (0600 file elsewhere).
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

Layout rule (enforced): a key prompt never sits on top of text, a button or another prompt, and a button's label always fits inside it. `tools/ui-lint.sh` renders every demo screen with `--ui-lint` and fails on any violation.

Options: `--shot out.png --press right,right,a` renders a scripted demo frame to PNG. See `--help`.
Fonts: JetBrains Mono and Archivo (OFL) in `assets/fonts`.

## Console image (Raspberry Pi)
`./image/build.sh` builds an SD-card image: stripped Raspberry Pi OS Lite that boots straight into Volcano OS, with controller support, networking, and native **Wi-Fi** and **Bluetooth** pages in the launcher (Start -> Network / Bluetooth). See [image/README.md](image/README.md).

## Status
Developed and tested on macOS. Linux/Windows code paths (sockets, process spawn, Chromium fallback) are written but untested.
