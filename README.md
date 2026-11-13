# Agent Observer

ESP-IDF firmware that shows what your AI coding agents (Claude Code, GitHub Copilot, or anything
that can run a shell command) are doing, on a small LCD with an animated robot and an RGB status LED.

Target board: **Waveshare ESP32-C6-LCD-1.47** (ST7789 172x320, WS2812 LED, BOOT button).
All board specifics live in `menuconfig`, so other ESP32 boards need no code changes.

## Features

- **Animated agent figure** per state: idle (breathing, looking around), working (typing on a
  keyboard, thought bubble), waiting for you (hopping, waving, "?" bubble), error, offline (sleeping).
  The figure is colored by state like the LED (green idle, red working, blinking yellow waiting, pink error,
  gray offline); Claude agents have one antenna and a visor, Copilot agents two antennas and round eyes.
- **RGB LED**: green = idle, red = working, blinking yellow = waiting for user action,
  fast red = error, breathing blue = no Wi-Fi, breathing cyan = no agents configured.
  By default the LED shows the most urgent state across *all* agents (configurable).
- **User button**: short press cycles through agents; long press (6 s) enters Wi-Fi setup.
- **Auto-focus**: an agent that starts waiting for the user is put on screen immediately
  (a manual button selection is respected for 15 s).
- **Web UI** (`http://agent-observer.local` or the IP shown on screen): add/rename/reorder agents,
  see live states, generate ready-to-paste hook configuration for Claude Code, VS Code Copilot,
  Copilot CLI, or plain curl.
- **Firmware updates over the air**: point the device at a GitHub repository/releases page (or a JSON
  manifest / direct `.bin` URL) and it checks periodically and installs new releases itself, with
  automatic rollback if the new firmware fails to come up.
- **Wi-Fi provisioning**: hold the button right after power-on (or when there are no saved
  credentials) to start an open access point `AgentObserver-XXXX` with a captive portal at
  `192.168.4.1` that scans networks and stores SSID/password in NVS.

## Build & flash

Requires ESP-IDF 5.1+ (tested with 5.4). Dependencies (`espressif/led_strip`, `espressif/mdns`)
are fetched automatically by the component manager.

```sh
idf.py set-target esp32c6
idf.py build flash monitor
```

> Windows note: the ESP-IDF toolchain cannot build from UNC/network paths. Copy or clone the project
> to a local drive before running `idf.py`.

### Other boards

Pins, panel geometry, LED type and button are all Kconfig options under
*Agent Observer* in `idf.py menuconfig`. Presets live in `boards/`:

```sh
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/lilygo_t_display_esp32.defaults" set-target esp32 build
```

Porting checklist:

1. LCD: set width/height, SPI pins, column/row gap, color inversion, rotation.
   For a non-ST7789 controller, add its `esp_lcd` driver component and change one line in
   [`main/display.c`](main/display.c) (`esp_lcd_new_panel_st7789`). All rendering is resolution-independent.
2. LED: addressable (WS2812/SK6812), three PWM GPIOs, or none.
3. Button: GPIO and polarity. On ESP32-C6/S3 the BOOT button is a strapping pin, so hold it *after*
   releasing reset; the boot screen gives a 3 s window with a progress bar.

## Provisioning

1. Power on while holding (or press within 3 s and hold for 1 s) the user button.
2. Join Wi-Fi `AgentObserver-XXXX` on your phone/PC. A captive-portal page opens (or browse to
   `http://192.168.4.1`).
3. Pick your network, enter the password, *Save & connect*. The device reboots and shows its IP.

Setup mode can also be entered from the web UI (*Wi-Fi setup...*) or by a 6 s button press.

Up to **5 networks** can be saved (setup portal, or the *Wi-Fi networks* card in the normal web UI,
which applies changes without a restart). The device scans and joins the *preferred* network when it is
in range, otherwise the strongest saved one; networks not seen in the scan (hidden SSIDs) are tried
last. When the connection drops it re-scans and may switch to another saved network. If none can be
reached it keeps retrying with back-off (up to 30 s); use setup mode to add a network on site.

## Connecting agents

1. In the web UI, add an agent with a display name and a short id (e.g. `backend`).
2. Open *Connect an agent*, pick the agent and the tab for your tool, copy the JSON.
3. Claude Code: merge into `~/.claude/settings.json` (see [`examples/claude-settings-hooks.json`](examples/claude-settings-hooks.json)).
   VS Code / Copilot CLI: save as `.github/hooks/agent-observer.json` in the repo.

The hooks POST the event payload to `http://<device>/api/hook/<id>?event=<Event>`.
Event mapping:

| Event | State |
|---|---|
| `SessionStart` | idle |
| `UserPromptSubmit`, `PreToolUse`, `PostToolUse`, `Subagent*` | working |
| `PreToolUse` for `AskUserQuestion` / `ExitPlanMode`, `PermissionRequest`, `Notification` | **waiting** |
| `Stop` | idle (or waiting, see `AO_STOP_IS_WAITING`) |
| `ErrorOccurred` | error |
| `SessionEnd` | offline |

Agents with no events for 2 h (configurable) are marked offline.

Hooks are push-only, so after a reboot the device cannot ask agents for their state. Instead it
saves every state change (coalesced over 10 s, and immediately before any software reboot such as
an OTA update) and restores it at boot, shown as *restored* until the agent sends its next event.
If the agent changed state while the board was off, the restored state is outdated until then.

## Firmware updates (OTA)

The flash is split into two app slots (`ota_0`/`ota_1`, see [`partitions.csv`](partitions.csv)); updates are
written to the inactive slot and activated on reboot.

In the web UI, *Firmware update* card:

- **Update source** – one of
  - `https://github.com/<owner>/<repo>` or `.../releases` – newest published release (via the GitHub API);
    `.../releases/tag/<tag>` pins a specific release.
  - a JSON manifest ending in `.json`: `{"version":"1.2.0","url":"https://example.com/agent_observer.bin"}`
  - a direct `.bin` URL (e.g. `https://github.com/<owner>/<repo>/releases/latest/download/agent_observer-esp32c6.bin`);
    the version is read from the image header before anything is written.
- **Asset name contains** – only needed when a release has several app `.bin` assets that the automatic
  choice cannot tell apart. Otherwise the asset whose name contains the chip (`esp32c6`, `esp32`, … as a whole
  word) is used, or the only `.bin` (bootloader / partition-table / merged images are ignored).
- **Check periodically and install automatically** + interval (1 h – 1 week). The first check runs
  1 minute after Wi-Fi connects. *Check now* only reports; *Update now* installs.

An update is installed only if it is newer than the running version (semantic versions like `v1.2.3` are
compared numerically; other version strings just have to differ), the image belongs to the same
project (`agent_observer`) and validates for this chip. Only `https://` sources are accepted
(certificates are checked against the ESP-IDF CA bundle).

**Dual-slot fallback:** the running firmware is never overwritten - updates go to the other slot, and
`otadata` only switches to it after the download completed and the image validated (an interrupted or
corrupt download leaves the device on the old version). The new image then boots in *pending verify*
state and is confirmed only after Wi-Fi has been up for 30 s without a crash (or the setup portal
opened). If it crashes, hangs into a watchdog reset or is power-cycled before that, the bootloader boots
the previous slot again. The failed version is remembered and not auto-installed again (a manual
*Update now* retries it). While a new image is still unconfirmed no further update is written, so the
known-good fallback is never lost. The web UI shows both slots and offers *Roll back to &lt;version&gt;*
to switch back manually.

### Publishing a release

1. Bump [`version.txt`](version.txt) (e.g. `1.1.0`) – this is the version embedded in the image.
2. `idf.py build`, then attach `build/agent_observer.bin` to a GitHub release tagged `v1.1.0`.
   For several boards, name the assets after the chip, e.g. `agent_observer-esp32c6.bin`,
   `agent_observer-esp32.bin`.

If a tag and the embedded version disagree (tag `v1.1.0` but the image says `1.0.0`), the device notices
after reading the image header, skips that release and does not download it again.

### Migrating from the single-app layout

Older builds used a single `factory` partition. Switching to OTA changes the partition table and enables
bootloader rollback, so flash **once over USB**: `idf.py build flash` (Wi-Fi credentials and agents in NVS
are kept). If you have an old `sdkconfig`, make sure `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`
(delete `sdkconfig` to regenerate it from `sdkconfig.defaults`). Every later update can go over the air.

## REST API

| Method | Path | Body |
|---|---|---|
| GET | `/api/agents` | – |
| POST | `/api/agents` | `{"id":"backend","name":"Backend","kind":"claude\|copilot\|other"}` |
| PUT | `/api/agents/<id>` | `{"name":"..","kind":".."}` or `{"move":-1\|1}` |
| DELETE | `/api/agents/<id>` | – |
| POST | `/api/hook/<id>?event=<Event>` | raw hook JSON (optional) |
| POST | `/api/state/<id>?state=working&detail=...` | or JSON `{"state":"waiting","detail":".."}` |
| GET | `/api/status` | – |
| POST | `/api/setup` | reboot into Wi-Fi setup |
| GET | `/api/wifi/networks` | saved networks (no passwords) |
| POST | `/api/wifi/networks` | `{"ssid":"..","password":"..","preferred":true}` add/update (omit `password` to keep it) |
| POST | `/api/wifi/networks/delete` | `{"ssid":".."}` |
| GET | `/api/ota` | update configuration and status |
| POST | `/api/ota` | `{"url":"https://github.com/o/r","asset":"","auto_update":true,"interval_h":24}` |
| POST | `/api/ota/check` | check for an update now |
| POST | `/api/ota/update` | check and install an update now |
| POST | `/api/ota/rollback` | boot the firmware in the other slot again |

States: `offline`, `idle`, `working`, `waiting`, `error`.

## Code layout

| File | Purpose |
|---|---|
| `main/main.c` | boot flow, mode selection, LED policy |
| `main/agents.c` | agent list, state machine, hook-event mapping, persistence |
| `main/ui.c` | screens and the animated robot (band-rendered, no framebuffer) |
| `main/gfx.c`, `font5x7.c` | small software rasterizer |
| `main/display.c` | `esp_lcd` ST7789 setup, double-buffered DMA bands |
| `main/status_led.c` | WS2812 / PWM LED patterns |
| `main/button.c` | debounce, short/long press |
| `main/wifi_mgr.c`, `dns_server.c` | STA with backoff reconnect, setup AP, captive DNS |
| `main/web_server.c`, `main/web/*.html` | HTTP API and embedded pages |
| `main/ota.c` | update source resolution (GitHub / manifest / binary), periodic check, install, rollback confirm |
| `main/Kconfig.projbuild` | all board/behaviour options |
