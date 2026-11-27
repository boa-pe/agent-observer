# Agent Observer

ESP-IDF firmware that shows what your AI coding agents (Claude Code, GitHub Copilot, Grok, or anything
that can run a shell command) are doing, on a small LCD with an animated robot and an RGB status LED.

Target board: **Waveshare ESP32-C6-LCD-1.47** (ST7789 172x320, WS2812 LED, BOOT button).
All board specifics live in `menuconfig`, so other ESP32 boards need no code changes.

## Features

- **Animated agent figure** per state: idle (breathing, looking around), working (typing on a
  keyboard, thought bubble), waiting for you (hopping, waving, "?" bubble), error, offline (sleeping).
  The figure is colored by state like the LED (green idle, red working, blinking yellow waiting, pink error,
  gray offline); Claude agents have one antenna and a visor, Copilot agents two antennas and round eyes, Grok agents a tilted antenna; each wears its logo on the belly.
- **RGB LED**: green = idle, red = working, blinking yellow = waiting for user action,
  fast red = error, breathing blue = no Wi-Fi, breathing cyan = no agents configured.
  The LED follows the agent on screen, including when you cycle agents with the button
  (menuconfig can switch it to the most urgent state across *all* agents).
- **User button**: short press cycles through agents; hold 5 s to unlock editing in the web UI; hold 10 s to enter
  Wi-Fi setup.
- **Automatic selection**: the display shows the most urgent online agent (waiting > error > working > idle).
  It is re-evaluated whenever any agent changes state (including going offline) and when a button selection
  expires; on a tie the agent on screen stays, then the one whose state just changed wins. An agent picked with
  the button stays on screen for 15 s, then automatic selection takes over again.
- **Web UI** (`http://agent-observer.local` or the IP shown on screen): add/rename/reorder agents,
  see live states, generate ready-to-paste hook configuration for Claude Code, VS Code Copilot,
  Copilot CLI, or plain curl.
- **Read-only web UI by default**: anyone on the network can watch agents and copy hook templates, but settings
  can only be changed after holding the device button for 5 s (the screen confirms *Editing enabled* and shows an
  *EDIT* tag while unlocked). Editing ends with *Log out* in the web UI or after 10 minutes without activity in
  the web UI (`CONFIG_AO_EDIT_HOLD_MS`, `CONFIG_AO_EDIT_TIMEOUT_MIN`). Edit mode applies to the whole device, not
  to one browser. Agent status (hooks, `/api/state`) is always accepted.
- **Browser wall** (`python viewer/server.py` → `http://127.0.0.1:8770/`): same robots and hook
  URLs as the LCD, as a grid in the browser (no ESP board required).
- **Several monitors in one network**: the mDNS name (`<name>.local`, default `agent-observer`) and the port
  that accepts agent status are set in the web UI (*Network* card) and stored on the device; the generated hook
  templates use both. Status is always accepted on port 80 as well, so the web UI and old hooks keep working.
- **Display brightness**: the backlight is PWM-dimmed; the level is set from the web UI (*Display* card)
  and kept across reboots. An optional inactivity timeout (0-60 min) drops the backlight to a separately set
  *Dimmed* level (default 50 %, never above the normal level) when no agent event has arrived for that long; the next event restores it. It never dims while an agent is waiting for
  user input; the countdown starts once no agent is waiting any more. The picture can be rotated by
  180° (*Rotation* in the same card), e.g. to mount the device upside down; applied immediately and on boot.
- **Power saving**: when every agent has been offline for 15 min (*Off after* in the *Display* card, 0-240 min,
  0 = never; kept across reboots), the display (backlight and panel) and the LED switch off and the device only keeps
  Wi-Fi alive. The next agent event switches everything on again; a button press does too (for at least a minute,
  without cycling agents). During normal operation the CPU scales its clock down and sleeps automatically between
  animation frames and Wi-Fi beacons, and the radio uses modem sleep. See [Power saving](#power-saving).
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

### Flashing with ESP Web Tool

For a first install from the browser, download the `agent-observer_webflasher.zip` file and unzip.
[latest GitHub release](https://github.com/boa-pe/agent-observer/releases/latest) 

Open the [ESP Web Tool](https://esptool.spacehuhn.com/) in a desktop Chromium-based browser, connect the board, and
select its serial port. Add each `.bin` file and enter the corresponding address from `flash-addresses.txt`, then
click **Program**. Keep the board connected until flashing completes. Later updates can go over the air
(see [Firmware updates](#firmware-updates-ota)).

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

Setup mode can also be entered from the web UI (*Wi-Fi setup...*) or by a 10 s button press (the hold passes the 5 s edit unlock on the way).

Up to **5 networks** can be saved (setup portal, or the *Wi-Fi networks* card in the normal web UI,
which applies changes without a restart). The device scans and joins the *preferred* network when it is
in range, otherwise the strongest saved one; networks not seen in the scan (hidden SSIDs) are tried
last. When the connection drops it re-scans and may switch to another saved network. If none can be
reached it keeps retrying with back-off (up to 30 s); use setup mode to add a network on site.

## Connecting agents

1. In the web UI, add an agent with a display name and a short id (e.g. `backend`).
2. Open *Connect an agent*, pick the agent and the tab for your tool, copy the JSON.
3. Claude Code: merge into `~/.claude/settings.json` (see [`examples/claude-settings-hooks.json`](examples/claude-settings-hooks.json)).
   VS Code / Copilot CLI: save as `.github/hooks/agent-observer.json` in the repo (see
   [`examples/vscode-hooks-agent-observer.json`](examples/vscode-hooks-agent-observer.json) and
   [`examples/copilot-cli-hooks-agent-observer.json`](examples/copilot-cli-hooks-agent-observer.json)).
   Grok: save as `~/.grok/hooks/agent-observer.json` or `<project>/.grok/hooks/agent-observer.json`
   (see [`examples/grok-hooks-agent-observer.json`](examples/grok-hooks-agent-observer.json)); project hooks
   must be trusted once with `/hooks-trust`.

The hooks send a bodyless POST to `http://<device>/api/hook/<id>?event=<Event>`, which answers an empty `200`.

**Privacy:** the hook payload (prompt, tool input and output, file paths, messages, transcript path) never leaves
the machine. Besides the event name, only two values are sent, and only for the events that need them:
the **tool name** (`&tool=Bash`) for `PreToolUse`, `PermissionRequest` and `PermissionDenied` (to recognise question
tools such as `AskUserQuestion` and to show what the agent is running), and the **notification type**
(`&ntype=permission_prompt`) for `Notification`. The hook extracts them locally from the JSON on stdin
(`grep`/`sed` in bash, `-match` in PowerShell) and keeps only `A-Z a-z 0-9 _ . : -`, at most 40 characters, so
nothing else can slip into the URL. All other events don't read stdin at all.
`/api/hook` still accepts a JSON body (for hooks created with older firmware and for scripts), but the generated
configuration never sends one.

The generated configuration works unchanged on Windows, Linux and macOS:

- **Claude Code** uses command hooks (`"type": "command"`) that run `curl` through bash (Git Bash on Windows,
  which Claude Code needs anyway). They run silently and always exit 0, so an unreachable device never fails a hook.
- **VS Code / Copilot CLI** hooks carry both a `bash` and a `powershell` command and the host picks the one for its
  platform. The PowerShell variant calls `curl.exe` (plain `curl` is an alias of `Invoke-WebRequest` in Windows
  PowerShell), reads the hook JSON via `$input` where it needs the tool name or notification type, discards the
  output with `Out-Null` (curl.exe cannot write to `/dev/null`) and always exits 0, because a non-zero `preToolUse`
  hook would deny the tool call in Copilot CLI. The bash variant ends in `|| true` for the same reason.
- **Grok** uses the same bash command hooks as Claude Code. Grok's HTTP hooks (`"type": "http"`) are not used,
  because they always POST the whole event JSON, tool input included. Grok hooks fail open (timeouts and errors
  never block a tool call). Grok also reads `.claude/settings.json`, so a project that already has Claude Code hooks
  for another agent reports to that agent from Grok sessions as well.

### Browser wall (no device)

Same agent figure and the **same hook configuration** as the LCD, shown as a grid on the laptop:

```sh
python viewer/server.py
```

Open `http://127.0.0.1:8770/`. In the generated hooks (or the files under [`examples/`](examples/)),
replace the device host with `127.0.0.1:8770` and keep `AGENT_ID` — the wall accepts
`POST /api/hook/<id>?event=…` (plus `&tool=` / `&ntype=` when needed), just like the firmware.
One card appears per agent id on the first event.

Event mapping:

| Event | State |
|---|---|
| `SessionStart` | idle |
| `UserPromptSubmit`, `PreToolUse`, `PostToolUse`, `PostToolUseFailure`, `PermissionDenied` | working |
| `SubagentStart`, `SubagentStop` | unchanged (Claude Code also runs short background agents after `Stop`) |
| `PreToolUse` for `AskUserQuestion` / `ExitPlanMode`, `PermissionRequest`, `Notification` | **waiting** |
| `Stop`, `agentStop` | idle (or waiting, see `AO_STOP_IS_WAITING`) |
| `ErrorOccurred`, `StopFailure` | error |
| `SessionEnd` | offline |

Agents with no events for 30 min are marked offline (which also triggers automatic selection). The timeout (0-240 min, 0 = never) is set in the web UI
(*Monitored agents* card, *Offline after*) and kept across reboots.

Agent states are not saved: after a reboot every agent shows as offline until it sends its next
event.

## Power saving

Enabled by `CONFIG_AO_POWER_SAVE` (menuconfig, *Agent Observer → Power saving*, on by default), which turns on
ESP-IDF power management (`CONFIG_PM_ENABLE`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE`):

- **CPU**: runs at full speed only while drawing a frame or handling a request, otherwise at the crystal frequency,
  and enters light sleep whenever all tasks are idle (between animation frames, and almost always in standby).
  The button is interrupt-driven and wakes the chip; the LED task only runs while its pattern animates.
- **Wi-Fi**: modem sleep (`WIFI_PS_MIN_MODEM`) instead of an always-on radio. The station still wakes for every DTIM
  beacon, so hook requests and mDNS lookups arrive, just up to a few hundred ms later (depends on the router's
  beacon/DTIM interval).
- **Backlight / LED PWM** are clocked from RC_FAST so they keep running in light sleep (ESP-IDF 5.4+; with older
  versions light sleep is only used while the backlight is off).
- **Standby** (all agents offline for the *Off after* time): backlight off, panel in sleep mode, LED off, no
  rendering, housekeeping once per second.

USB Serial/JTAG (the USB-C port of ESP32-C6 boards) does not work during light sleep. By default
`CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION` is enabled with power saving, so light sleep is skipped while a USB host is
connected: flashing and `idf.py monitor` keep working, and the device sleeps on a USB charger or while the PC is
suspended. Disable that option for the lowest consumption on PC power (then enter download mode with the BOOT
button to flash, or use OTA). Boards with a USB-UART bridge (e.g. LILYGO T-Display) are not affected.

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

  Defaults to `https://github.com/boa-pe/agent-observer/releases/latest` (`CONFIG_AO_OTA_URL`) until a URL
  is saved; saving an empty field disables update checks.
- **Asset name contains** – only needed when a release has several app `.bin` assets that the automatic
  choice cannot tell apart. Otherwise the asset whose name contains the chip (`esp32c6`, `esp32`, … as a whole
  word) is used, or the only `.bin` (bootloader / partition-table / merged images are ignored).
  Defaults to `agent-observer-fota-esp32c6-lcd147` (`CONFIG_AO_OTA_ASSET`) until the settings are saved.
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
2. `idf.py build`, then attach `build/agent_observer.bin` to a GitHub release tagged `v1.1.0`
   (any prefix works: `release_1.1.0`, `fw-1.1.0`; the first `x.y[.z]` in the tag is the version).
   For several boards, name the assets after the chip, e.g. `agent_observer-esp32c6.bin`,
   `agent_observer-esp32.bin`.
3. For first-time installs from the browser, also attach the files from `.\scripts\build_release.ps1`
   (`build\web-flasher`: all `.bin` files and `flash-addresses.txt`, see
   [Flashing with ESP Web Tool](#flashing-with-esp-web-tool)).

If a tag and the embedded version disagree (tag `v1.1.0` but the image says `1.0.0`), the device notices
after reading the image header, skips that release and does not download it again.

### Migrating from the single-app layout

Older builds used a single `factory` partition. Switching to OTA changes the partition table and enables
bootloader rollback, so flash **once over USB**: `idf.py build flash` (Wi-Fi credentials and agents in NVS
are kept). If you have an old `sdkconfig`, make sure `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`
(delete `sdkconfig` to regenerate it from `sdkconfig.defaults`). Every later update can go over the air.

## REST API

Requests that change settings (all `POST`/`PUT`/`DELETE` below except `/api/hook`, `/api/state` and `/api/session/*`)
are refused with `403 {"error":"read-only mode: ...","read_only":true}` unless edit mode is on (button held 5 s).
Each accepted change restarts the 10 min inactivity timeout.

| Method | Path | Body |
|---|---|---|
| GET | `/api/agents` | – |
| POST | `/api/agents` | `{"id":"backend","name":"Backend","kind":"claude\|copilot\|grok\|other"}` |
| PUT | `/api/agents/<id>` | `{"name":"..","kind":".."}` or `{"move":-1\|1}` |
| DELETE | `/api/agents/<id>` | – |
| POST | `/api/hook/<id>?event=<Event>` | raw hook JSON (optional); replies with an empty `200` |
| POST | `/api/state/<id>?state=working&detail=...` | or JSON `{"state":"waiting","detail":".."}` |
| POST | `/api/agents/settings` | `{"offline_timeout_min":0..240}` mark agents offline after that many minutes without events (0 = never); also reported by `GET /api/agents` |
| POST | `/api/network` | `{"hostname":"agent-observer-2","hook_port":8080}` store mDNS name and agent status port, then restart (`GET /api/status` reports both) |
| GET | `/api/status` | – |
| GET | `/api/events` | last 32 hook events as received, newest first: `{"events":[{"age_ms":1200,"id":"backend","event":"Stop","session":"8536c316","from":"working","to":"idle"}]}` (`to` is `ignored` for unknown events / agents). Also in the web UI under *Monitored agents → Recent hook events* |
| POST | `/api/setup` | reboot into Wi-Fi setup |
| GET | `/api/wifi/networks` | saved networks (no passwords) |
| POST | `/api/wifi/networks` | `{"ssid":"..","password":"..","preferred":true}` add/update (omit `password` to keep it) |
| POST | `/api/wifi/networks/delete` | `{"ssid":".."}` |
| GET | `/api/display` | `{"brightness":100,"adjustable":true,"dim_brightness":50,"dim_timeout_min":0,"dimmed":false,"rotation":0}` |
| POST | `/api/display` | `{"brightness":5..100,"dim_brightness":5..100,"dim_timeout_min":0..60,"rotation":0\|180}` (any field) apply and store; add `"preview":true` to apply (show) the brightness / dimmed level without storing |
| GET | `/api/ota` | update configuration and status |
| POST | `/api/ota` | `{"url":"https://github.com/o/r","asset":"","auto_update":true,"interval_h":24}` |
| POST | `/api/ota/check` | check for an update now |
| POST | `/api/ota/update` | check and install an update now |
| POST | `/api/ota/rollback` | boot the firmware in the other slot again |
| GET | `/api/session` | `{"edit":false,"remaining_s":0,"timeout_min":10,"hold_s":5}` edit mode state (`GET /api/agents` also reports `edit`) |
| POST | `/api/session/ping` | web UI activity: restarts the inactivity timeout while edit mode is on (never turns it on) |
| POST | `/api/session/logout` | end edit mode (back to read-only) |

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
| `main/button.c` | debounce, short press, edit-unlock hold, setup long press |
| `main/wifi_mgr.c`, `dns_server.c` | STA with backoff reconnect, setup AP, captive DNS |
| `main/web_server.c`, `main/web/*.html` | HTTP API and embedded pages |
| `main/ota.c` | update source resolution (GitHub / manifest / binary), periodic check, install, rollback confirm |
| `main/Kconfig.projbuild` | all board/behaviour options |
