# MeshCore → TAK Gateway

Firmware for a **Heltec WiFi LoRa 32 V3** that listens to a [MeshCore](https://github.com/meshcore-dev/MeshCore) mesh and puts it on a **TAK Server**: nodes that advertise GPS become map markers, and MeshCore channel chat is bridged both ways with TAK GeoChat rooms. It connects over TLS using the client certificates from a **TAK Portal Integration**, and is set up entirely from a web page served by the device.

This repository is a fork of MeshCore. The gateway lives in [`examples/tak_gateway/`](examples/tak_gateway/); everything else is upstream MeshCore (see [README.MeshCore.md](README.MeshCore.md)).

## Features

- **Map markers** — every MeshCore advert that carries a location becomes a CoT marker with one stable uid per node (latest position, not a trail). Optional name-prefix filter (e.g. only `tak_*` nodes), with the prefix optionally stripped from the callsign.
- **Marker styling** — 2525 symbols, TAK Default iconset icons (render the same in ATAK, CloudTAK and TAK Portal), or spot-map dots with color; stale time, remarks, archive flag, and a live CoT preview.
- **Chat bridge** — up to 3 MeshCore channels (private key or `#hashtag`) mapped to TAK chat rooms, both directions. Optionally mirrors the MeshCore Public channel into a TAK room (listen only). The gateway appears in TAK as a contact that can be messaged directly. The last 3 bridged messages are shown on the page and the OLED.
- **Mesh advert** — the gateway can flood its own MeshCore advert with a name and location so it shows up in contact lists and on mesh maps, on a schedule or on demand.
- **Customization** — page title (also shown on the OLED), identification banner, accent color and logo.
- **OLED status pages** — TAK link, Wi-Fi, radio, nodes, chat.

## Hardware

- Heltec WiFi LoRa 32 **V3** (ESP32-S3 + SX1262 + SSD1306 OLED)
- A 2.4 GHz Wi-Fi network with internet access (NTP and the TAK Server)

## Build and flash

Install [PlatformIO](https://platformio.org/), then from the repository root:

```bash
pio run -e Heltec_v3_tak_gateway -t upload
```

For a first flash or recovery, build a merged image and write it at `0x0`:

```bash
pio run -e Heltec_v3_tak_gateway -t mergebin
```

Images are written to `.pio/build/Heltec_v3_tak_gateway/`.

If you edit the setup page (`examples/tak_gateway/web/index.html`), regenerate the embedded assets before building (needs Python with Pillow; icon data is downloaded from CloudTAK on first run):

```bash
python examples/tak_gateway/tools/build_web_assets.py
```

## Versions and updates

The version is the UTC (Zulu) time the code was finished, `YY.MMDD.HHMM`; for example `26.1002.1349` is 2 Oct 2026 13:49Z. It is shown on the setup page, the OLED at boot, the serial log and in the gateway's TAK contact.

- A build from a clean checkout takes the time of the last commit, so it matches the GitHub release of that commit. A build with uncommitted changes takes the build time, so every modified build gets a new number.
- Every push to `main` that touches the gateway publishes a GitHub release with that version (`.github/workflows/tak-gateway-release.yml`). It contains `tak_gateway_heltec_v3.bin` (over-the-air update), `tak_gateway_heltec_v3_full.bin` (USB flash at `0x0`) and `version.txt`.
- The gateway checks the latest release a minute after boot and every 6 hours. When a newer one exists, the dashboard shows it; **Install update** downloads it from GitHub, verifies it and restarts. Settings, keys and certificates are kept. You can also upload a `.bin` under **System → Firmware**.
- Downloads are checked against the GitHub root certificates in `tak/TakUpdateRoots.h`. If GitHub changes certificate authority, regenerate it with `python examples/tak_gateway/tools/make_update_roots.py`.

## Setup

1. Power the gateway. With no Wi-Fi configured it starts an access point **`MeshCore-TAK-Setup`** (password `meshcoretak`). Join it and open <http://192.168.4.1>. Sign in as **`admin`** with password **`meshcore`**, and set your own password under **System**.
2. **Connection** — enter your Wi-Fi network and the TAK Server host and port. Once Wi-Fi joins, the page is also served on the device's LAN address (shown on the OLED).
3. **Portal certificates** — in TAK Portal, create an Integration, assign it a `*_WRITE` group and **Download Certs**. Select the `.pem` and `.key` on the setup page and install. Use the host and port shown for your Integration.
   - Portal keys are usually encrypted with 3DES (passphrase `atakatak`), which the ESP32 cannot decrypt. If installing from the page fails, install from a PC instead (it asks for the web password):
     ```bash
     pip install cryptography
     python examples/tak_gateway/tools/install_certs_to_device.py <device-ip> <unzipped-cert-folder>
     ```
4. **MeshCore radio** — pick the preset that matches your mesh (US: 910.525 MHz, BW 62.5, SF 7, CR 5) or enter custom values.
5. Enable **Send to TAK Server** and save. The status pill turns green when connected, and the Portal Integration shows Connected.

Users subscribed to the matching `*_READ` group see the markers and chat.

### Chat channels

- Channel keys are entered on the setup page and stored only on the device; the API never returns them. Paste the 32-hex (or base64) secret from the MeshCore app's Share Channel screen, or use a `#hashtag` channel name, which needs no key.
- Each channel maps to a TAK chat room (defaults to the channel name). Messages from TAK are sent on the mesh as `callsign: message`.

### Trackers

Any MeshCore node that includes its location in adverts will appear, for example a T-Beam or companion radio with GPS and advert location sharing enabled. Use a short advert interval for live movement.

## Device controls

- **Button** — click cycles OLED pages; long press prints the factory-reset hint.
- **Serial (115200)** — `status`, `factory_reset`.

## Security notes

- Client certificates, keys and chat channel secrets are stored in the device's flash only. Nothing secret is compiled into the firmware or kept in this repository; `.gitignore` excludes `*.key`, `*.p12` and `examples/tak_gateway/certs_converted/`.
- The whole setup page and API require a login (HTTP digest auth, user `admin`). Change the default `meshcore` password on first setup; the page warns until you do. The password is never sent back to the browser. Forgot it? Run `factory_reset` over serial.
- The page is plain HTTP, so other settings still travel unencrypted on your LAN. Keep the gateway on a trusted network. The setup access point uses the default password above and shuts off once the gateway joins Wi-Fi.

## Source layout

| Path | Role |
|------|------|
| `examples/tak_gateway/main.cpp`, `MyMesh.*` | Entry point, mesh listener, chat channels, adverts |
| `examples/tak_gateway/tak/` | Config, node table, CoT builder, TLS client, web server, OLED |
| `examples/tak_gateway/web/index.html` | Setup page source (embedded via `TakWebAssets.h`) |
| `examples/tak_gateway/tak/TakUpdate.*` | Update check and install from GitHub releases |
| `examples/tak_gateway/tools/` | Asset builder, version stamp, update root CAs and Portal certificate helpers |
| `variants/heltec_v3/platformio.ini` | `Heltec_v3_tak_gateway` build environment |

## Credits and license

- [MeshCore](https://github.com/meshcore-dev/MeshCore) by the MeshCore developers — MIT, see [license.txt](license.txt).
- Marker icons from [CloudTAK](https://github.com/dfpc-coe/CloudTAK) and the [CloudTAK-Data](https://github.com/dfpc-coe/CloudTAK-Data) Default iconset.
- Gateway code is released under the same MIT license.
