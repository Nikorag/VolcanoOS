# Volcano OS SD-card image

Builds a flashable image: Raspberry Pi OS Lite (bookworm, 64-bit) with the desktop
stripped out, booting straight into the Volcano OS launcher. Supports Raspberry Pi
3, 4, 5 and Zero 2 W.

What's in it:

- **No passwords**: it boots straight into the launcher with no login prompt, and the `volcano` account has no password.
- **Boot**: `volcano.service` runs [cage](https://github.com/cage-kiosk/cage) (a single-app Wayland compositor) on tty1 with the launcher inside. No login prompt, no desktop. If the launcher exits or crashes it restarts.
- **Controllers**: USB and Bluetooth pads work through SDL. The launcher's **Bluetooth** page pairs them (`bluetoothctl`); `uhid` is loaded and ERTM is disabled so Xbox pads behave.
- **Networking**: NetworkManager. The launcher's **Wi-Fi** page scans, joins (password via the on-screen keyboard), joins hidden networks and forgets networks. Ethernet works with no setup. On first boot with no network the Wi-Fi page opens by itself.
- **Web games and sign-in**: Chromium (Wayland), launched by the launcher. XWayland is installed for X11-only native games.
- **SD-card friendly**: logs in RAM, no swap file, Wi-Fi power saving off.

## Build

Needs Docker (Docker Desktop on macOS, or a Linux machine), about 15 GB free, and a
first build takes 30 to 90 minutes.

    ./image/build.sh

The result is `image/out/<date>-volcano-os.img.xz`. Options (environment variables):

| Variable | Default | |
|---|---|---|
| `VOLCANO_COUNTRY` | `GB` | Wi-Fi regulatory country. Wrong values hide channels. |
| `VOLCANO_TZ` | `Europe/London` | Timezone |
| `VOLCANO_SSH` | `0` | `1` enables the SSH server (there is no password, so add an SSH key, e.g. via Raspberry Pi Imager's OS customisation) |
| `PIGEN_REF` | `bookworm-arm64` | pi-gen branch to build with |

The launcher source is snapshotted from your working tree (tracked and untracked,
minus gitignored files) and compiled inside the image, so a build always matches
what you have checked out.

## Flash

Raspberry Pi Imager: *Choose OS -> Use custom*, pick the `.img.xz`, choose the card.
Or from a terminal (macOS: find the disk with `diskutil list`, then
`diskutil unmountDisk /dev/diskN`):

    xzcat image/out/*-volcano-os.img.xz | sudo dd of=/dev/rdiskN bs=4m status=progress   # macOS
    xzcat image/out/*-volcano-os.img.xz | sudo dd of=/dev/sdX bs=4M status=progress      # Linux

Put the card in the Pi, plug in HDMI and a controller, power on. The root partition
grows to fill the card on first boot.

## Using it

1. First boot with no network: the **Wi-Fi** page opens. Pick your network with the d-pad, press A, type the password.
2. Back out (B), press A on **Sign in**. Sign-in opens Chromium; see the limitation below.
3. Anytime: Start (or the Guide button) -> **Network** / **Bluetooth**. Controller not responding? Hold its pairing button and use the **Bluetooth** page from another controller or a keyboard.

## Known limitations

- **Sign-in needs a keyboard once.** The Zonto login happens in a browser, and a browser has no controller-driven on-screen keyboard. Plug in a USB keyboard for the first sign-in; the token is kept afterwards. A proper fix is a device-code / QR sign-in endpoint, see `docs/API-GAPS.md`.
- **Native games must exist for `linux/aarch64`.** Web games work everywhere.
- **Bluetooth keyboards/mice that need a PIN** can't be paired from the launcher (controllers use "just works" pairing and are fine). Pair those from a console with `bluetoothctl` (Ctrl+Alt+F2 on a USB keyboard, log in as `volcano`, no password).
- Enterprise (802.1X) Wi-Fi isn't supported.
- **Not yet tested on hardware.** The launcher compiles on Debian bookworm arm64 and the Wi-Fi/Bluetooth parsing is tested against fake `nmcli`/`bluetoothctl` output, but this image recipe has not been built end to end or booted on a Pi. Expect to iterate on the first build.

## Debugging

    journalctl -u volcano -b        # launcher + cage output (on a USB keyboard: Ctrl+Alt+F2, log in)
    nmcli device wifi list
    bluetoothctl devices

Launcher logs for games and the browser are under `~/.local/share/zonto/volcanoos/logs`.

## Building in CI / on a self-hosted runner

`.github/workflows/image.yml` runs `image/build.sh` and uploads the image as an artifact
(manual trigger). It has not been run yet. For a k3s cluster using actions-runner-controller:

- Use a runner scale set in **dind** mode with `privileged: true` (pi-gen needs loop devices and qemu `binfmt_misc`).
- The nodes need the `loop` kernel module loaded.
- Give runners about 30 GB of ephemeral storage and 4+ GB RAM.
- On x86 nodes the arm64 image is built under qemu (the workflow registers it); expect several hours for the first build.
- Set the repo variable `IMAGE_RUNNER` to the scale set name. Otherwise it uses GitHub's free-for-public-repos `ubuntu-24.04-arm` runner, which builds natively (no qemu) and is the quickest route.
