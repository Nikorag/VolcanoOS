# Testing Volcano OS in UTM (no Pi needed)

The Pi image is Debian bookworm arm64 plus the files in `image/stage-volcano/`, so a Debian 12 arm64
VM provisioned with the same files behaves the same way: same SDL2/SDL_ttf/FreeType versions, same
cage + Chromium + NetworkManager + Plymouth setup. On an Apple Silicon Mac it runs near-native.

## One-time setup

1. UTM -> **Create a New Virtual Machine** -> **Virtualize** -> **Linux**.
   Use the [Debian 12 arm64 netinst ISO](https://www.debian.org/distrib/netinst) (arm64).
   4 GB RAM, 4 cores, 16 GB disk.
2. Before first boot, edit the VM -> **Display**: set the emulated display card to **virtio-gpu-gl-pci**
   (GPU-accelerated; the same Mesa/GLES path as the Pi). Add a **Sound** device if you want audio.
3. Install Debian: untick every desktop environment; keep only **SSH server** and **standard system utilities**.
4. Log in on the console, then:
   ```
   sudo apt-get install -y git
   git clone https://github.com/Nikorag/VolcanoOS && cd VolcanoOS
   sudo ./utm/provision.sh
   sudo reboot
   ```
   It reboots into the boot animation, then Volcano OS fullscreen, like the Pi.

## Iterating

Edit code on your Mac, push, then in the VM (SSH in with `ssh user@<vm-ip>`; UTM shows the IP):

```
cd VolcanoOS && git pull && sudo ./utm/provision.sh --update
```

That rebuilds and restarts only the launcher (seconds, not a 30-minute image build).
Logs: `journalctl -u volcano -b`, and `~volcano/.local/share/zonto/volcanoos/logs/` (`volcano.log`, `web.log`).

## What it can't test

- **Wi-Fi and Bluetooth.** The VM only has a virtual Ethernet link, so the Wi-Fi/Bluetooth pages show
  "no adapter". (To test those flows, plug a USB Wi-Fi/Bluetooth dongle into the Mac and pass it to the VM
  via UTM's USB menu.) With Ethernet up, the console goes straight to the phone sign-in code + QR.
- Pi-specific GPU quirks (vc4/v3d) and the Pi firmware boot stages.
