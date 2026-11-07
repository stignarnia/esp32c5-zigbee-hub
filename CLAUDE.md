# CLAUDE.md — ESP32-C5 Local Zigbee Hub (plan)

This file is the **plan and working notes**. `README.md` describes the finished
product as if every phase below were done; when the two disagree, this file is the
truth about what actually exists.

## Goal

One ESP32-C5 board that is, on its own:

1. **Provisioned by the flash script** — it joins the Wi-Fi network the flashing
   machine is connected to, **strictly on 5 GHz**. 2.4 GHz Wi-Fi is disabled in
   firmware; the 2.4 GHz band belongs to Zigbee.
2. **A web UI** served by the board itself.
3. **A Zigbee 3.0 coordinator**, with device enrollment driven from the UI
   (scan / permit-join, live discovery, interview, naming).
4. **A Tailscale node** on the user's tailnet, so the UI is reachable from anywhere.
5. **A local automation engine** — scripts in an interpreted language (Lua),
   pasted into the UI and run on the board, with no cloud and no Home Assistant.
6. **Optional Home Assistant integration** — an opt-in setting (off by default).
   When it's on, the board speaks the **ESPHome native API**: with no script
   enabled, every Zigbee device appears in Home Assistant as a native sub-device of
   the hub; with scripts enabled, the hub appears as one smarter device (see Phase 6).

## Priorities (user decision, 2026-11-04)

**Zigbee + local automation must be rock solid. Wi-Fi is allowed to drop.**

Everything below follows from that ordering:

1. **Zigbee and the automation engine never depend on Wi-Fi or Tailscale.** They
   start before Wi-Fi, keep running with no uplink, and nothing in their paths
   blocks on the network.
2. **A Wi-Fi reboot is a last resort, taken only at a safe moment.** The reference
   repo found the C5 can get stuck not finding an AP that is right there, and only
   a reboot was proven to bring it back. So the escalation ends in a reboot, but
   it is gated on the automation/Zigbee side being idle (see the Wi-Fi watchdog in
   Phase 1), and pending timers survive it.
3. **Coex preference favours 802.15.4.** With Wi-Fi on 5 GHz there's no band
   overlap, but the C5 shares RF resources; when they contend, Zigbee wins.
4. **Watchdog and memory budgets are set by the Zigbee/automation side.** The UI
   and Tailscale get what's left; if memory is short, Tailscale buffers shrink
   first.
5. **Automation side effects that touch the network never block a script.**
   HTTP requests are queued to their own task; results come back as callbacks.
   A dead uplink delays them (or, with `retry_for`, holds them until it's back) but
   can't stall a trigger or timer — the local buzzer still sounds at once.
6. **The HA integration doesn't change the priority.** The ESPHome API server is a
   consumer of the device registry like the UI; it must never block Zigbee. In devices
   mode (no local scripts) automations live in HA and depend on Wi-Fi by definition —
   the user's choice; with local scripts, they run on the hub whatever HA does.

## Ground rules

- **Reference repos are inspiration only.** `vendor/zigbee2esphome` and
  `vendor/microlink` are gitignored clones. Read them for ideas and measured
  facts; never build them, never follow their install steps. Code that is
  reused gets copied into `components/` with its license, deliberately.
- **Everything stays in this folder.** The toolchain lives in `.esp/`
  (gitignored), activated by `source ./env.sh`. Nothing in `~/.espressif`,
  `~/.cache/Espressif` or system packages.
- **No system Python.** Python is uv-managed (`.esp/python`, `.esp/venv`).
  Host-side helpers that need Python run with the venv's interpreter or `uv run`.
- **Measure on hardware before claiming.** The reference repo is full of
  conclusions that were wrong until measured. Every "done" below needs a serial
  log or a UI observation behind it.
- **Always the latest stable version of everything** (user rule). ESP-IDF,
  managed components, vendored code (Lua, microlink's deps), host tools. Check
  upstream before adding anything; never inherit a version from a reference
  repo. Old code gets ported forward, not pinned.

## Toolchain (done, 2026-11-04)

| | |
|---|---|
| ESP-IDF | **v6.1**, shallow clone in `.esp/esp-idf` |
| Compiler | `riscv32-esp-elf-gcc` 15.2.0 in `.esp/tools` |
| Python | CPython 3.14.10 via uv, venv `.esp/venv`, deps via `uv pip` + Espressif constraints |
| Activate | `source ./env.sh` (sets `IDF_*`, `UV_*`, `ESPPORT`, runs `export.sh`) |
| Verified | hello_world built, flashed, ran (`.esp/smoke-test`) |

Serial monitor without a TTY (what Claude uses):

```bash
stty -F /dev/ttyACM0 115200 raw -echo && timeout 15 cat /dev/ttyACM0
```

`idf.py monitor` needs an interactive terminal — the user can run it with `! idf.py monitor`.

## Hardware (read from the chip)

| | |
|---|---|
| SoC | ESP32-C5HR8, rev v1.0, single-core RISC-V 240 MHz |
| Flash | 16 MB external |
| PSRAM | 8 MB embedded |
| Radios | Wi-Fi 6 2.4+5 GHz, BLE 5, IEEE 802.15.4 |
| USB-UART | CH343 (`1a86:55d3`), `/dev/serial/by-id/usb-1a86_USB_Single_Serial_*-if00` (env.sh finds it) |
| Console | UART0 on GPIO 11/12, 115200 |

The user is in group `uucp` (serial access).

## The flashing machine's network (observed 2026-11-04)

The home network: WPA2/WPA3 mixed, **5 GHz channel 52 (5260 MHz) — a DFS channel.**
The reference repo measured the C5 on DFS channel 64: API logins took 10–28 s and
often timed out; on channel 48 they took 0.8–2.6 s. Espressif documents that the
C5 cannot detect radar or vacate DFS channels.

**Decision:** test on DFS channel 52 first — the reference measured channel 64 with
older firmware, and nobody knows how 52 behaves with ours. Measure time-to-IP,
disconnect reasons (esp. 201 NO_AP_FOUND) and HTTP latency over a long run. Only if
it's bad does the user move the home network to 36/40/44/48 (they control the router).
The flash script warns on DFS channels but doesn't refuse.

Visible 2.4 GHz neighbours: Wi-Fi channels 6, 8, 9, 11 → Zigbee channels 15 and
25 are the clean ones.

## Tailnet

The user's personal tailnet (not a work account). The board will be
`zigbee-hub.<tailnet>.ts.net`; the dev machine is on the same tailnet, so
`tailscale ping zigbee-hub` works from it for verification. Concrete names
(network, tailnet, machines, device identifiers) are in `CLAUDE.local.md`
(gitignored, not published).

## Architecture

```
            ┌──────────────────── Web UI (static, gzipped in flash) ───────────────┐
            │  Devices · Enroll · Automations · Network · Tailscale · System       │
            └──────────────────────────────┬───────────────────────────────────────┘
                              REST + WebSocket (esp_http_server, port 80)
            ┌──────────────────────────────┴───────────────────────────────────────┐
            │                             Event bus                                │
            ├──────────────┬────────────────┬─────────────────┬────────────────────┤
            │  Zigbee      │  Device        │  Automation     │  Tailscale         │
            │  coordinator │  registry      │  engine (Lua)   │  (microlink port)  │
            │  zb_*        │  + persistence │  ── and ──      │  WireGuard netif   │
            │              │  (LittleFS)    │  ESPHome API    │  in lwIP           │
            │              │                │  (HA, :6053)    │                    │
            ├──────────────┴────────────────┴─────────────────┴────────────────────┤
            │  Wi-Fi manager (STA, 5 GHz only)  ·  coex  ·  provisioning (NVS)     │
            └──────────────────────────────────────────────────────────────────────┘
```

The HTTP server binds to all interfaces. microlink adds its WireGuard interface to
lwIP's netif list, so the same server answers on the LAN IP and on the `100.x`
tailnet IP without extra code (to verify in Phase 4).

### Source layout (as built, 2026-11-07)

```
CMakeLists.txt  sdkconfig.defaults  partitions.csv  flash.sh  env.sh
main/            app_main, boot order, Wi-Fi watchdog's reboot gate
components/
  prov/          read provisioning NVS partition
  wifi5g/        STA manager, 5 GHz only, watchdog
  hubsys/        planned reboots, OTA bookkeeping, core-dump summary
  hublog/        on-board diagnostic log (LittleFS)
  zb/            coordinator, enrollment, interview, ZCL helpers, and the device
                 registry with persistence (zb_dev.c; no separate devices/ component)
  web/           http server, REST (web_api.c, web_auto.c), WebSocket, auth, access mode
  web/ui/        index.html, app.js, style.css (no build step, gzipped by CMake), and
                 cm.js (CodeMirror editor, prebuilt by tools/codemirror, committed)
  automation/    Lua engine (auto_engine.c), store, GPIO, HTTP
  lua/           Lua 5.5.1 source (MIT), vendored unmodified
  tailscale/     microlink (MIT) ported to IDF v6, wrapped
  esphome_api/   (Phase 6, not yet) ESPHome native API server (Noise), HA mode
tools/
  mkprov.py      provisioning image for flash.sh
  gpio_probe/    Phase 1b buzzer/GPIO probe firmware
  codemirror/    editor bundle: pinned package.json + lockfile, entry.js, build.sh, LICENSE
```

### Partition table (16 MB)

Eight partitions, contiguous. Small ones packed first, ending on the 64 KB boundary
the app slots need; app slots and LittleFS take everything else (2026-11-06).

| Name | Type | Offset | Size | Purpose |
|---|---|---|---|---|
| nvs | nvs | 0x9000 | 0x4000 | app settings, Wi-Fi driver state, radio calibration (~3 KB used, measured) |
| otadata | ota | 0xD000 | 0x2000 | |
| prov | nvs | 0xF000 | 0x3000 | **written by flash.sh**: SSID, PSK, admin password, hostname, `lanreset` nonce, optional HA setting + API key (< 1 KB). The Tailscale auth key is entered in the UI (app NVS) |
| zb_storage | nvs | 0x12000 | 0x1E000 | Zigbee stack NVRAM (network key, PAN, children). esp-zigbee-lib 2.x needs NVS, not FAT. Generous on purpose: resizing it later means re-pairing every device |
| coredump | coredump | 0x30000 | 0x10000 | ELF core dump on panic; summary logged on next boot, `/api/coredump` |
| ota_0 | app | 0x40000 | 0x400000 | |
| ota_1 | app | 0x440000 | 0x400000 | |
| storage | littlefs | 0x840000 | 0x7C0000 | device registry, scripts, logs, UI overrides |

Credentials live in their own partition so re-provisioning never touches the
Zigbee network (re-pairing every device would be the cost) or app settings.

## Phases

Status markers: `[ ]` todo · `[~]` in progress · `[x]` done and verified on hardware.

### Phase 0 — Skeleton (done 2026-11-04)
- [x] Project `CMakeLists.txt`, `sdkconfig.defaults` (16 MB flash, PSRAM quad
      80 MHz + malloc, custom partitions, 240 MHz, app rollback in bootloader).
      WPA3 setting moves to Phase 1 together with Wi-Fi
- [x] `partitions.csv` — boot log listed the partitions at the planned offsets.
      (Re-laid out in Phase 3 to the 8-partition table above; boot log confirms it)
- [x] Boot log: `flash: 16 MB`, `esp_psram: Found 8MB PSRAM device`,
      `Adding pool of 8192K of PSRAM memory to heap allocator`
- [x] Baseline (skeleton only): app image 218 KB (5 % of a 4 MB slot);
      HP SRAM static 68 KB of 321 KB; heap at boot **313 KB internal free**
      (largest block 252 KB), **8189 KB PSRAM free**

Notes from the first boot:
- The PSRAM self-test cost ~1 s of boot (`psram timing tuning` at 191 ms →
  `memory test OK` at 1238 ms). **Disabled** (`CONFIG_SPIRAM_MEMTEST=n`, user
  request) after it passed on every boot; app now starts at 0.49 s.
- Rev v1.0 silicon: **PSRAM is not encrypted even with flash encryption**, and
  IDF warns against TLS allocations in PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`).
  We don't plan flash encryption, but Phase 4 must decide deliberately where
  microlink's TLS/WireGuard state lives (keys internal, bulk buffers in PSRAM).

### Phase 1 — Provisioning + 5 GHz Wi-Fi (done 2026-11-04; 42 h soak passed 2026-11-06)
- [x] `flash.sh` (+ `tools/mkprov.py`):
  - finds the active Wi-Fi connection with `nmcli`; reads SSID + PSK (sudo
    fallback only if NetworkManager withholds the secret — here it didn't)
  - rejects open / enterprise networks (WPA2/WPA3-Personal only)
  - **refuses** if the SSID is only visible on 2.4 GHz; **warns** on DFS (52–144)
  - `--backup-ssid/--backup-psk` (or `--no-backup`), `--hostname`; remembered in
    `.esp/secrets/prov.env` (0600) for the next run. `--lan-access` (one-shot, not
    remembered) turns tailnet-only access off (Phase 4)
  - admin password generated once (`.esp/secrets/admin_password`); only a
    PBKDF2-SHA256 hash (10k iters, random salt) goes to the board
  - country code from `iw reg get` (here `IT`) → `esp_wifi_set_country_code`
  - builds, creates `prov.bin` (secrets passed via env, never argv; temp CSV 0600,
    deleted), flashes app + prov in one esptool call, waits for `HUB_READY`
  - `--prov-only` re-writes credentials without rebuilding
  - [x] `--ota [--host H]` updates over the network instead of USB (board side in
    Phase 2; see "Remote firmware updates")
- [x] `wifi5g`: band set to `WIFI_BAND_MODE_5G_ONLY` **after** `esp_wifi_start()`
      (the API returns `ESP_ERR_WIFI_NOT_STARTED` before) and **before** the first
      connect — nothing connects on `STA_START`. Re-applied on every driver
      restart. **No fallback to 2.4 GHz, ever.** Association on channel < 36 is
      refused defensively
- [x] `threshold.authmode = WIFI_AUTH_WPA2_PSK` (a floor); the home network negotiates
      **WPA3-SAE with PMF** (`auth 6`, `pmf:1`)
- [x] PMF capable, not required; power save off (mains powered)
- [x] **Primary + backup network** (user request): if the current network is
      unreachable for 2 min, the other is tried, alternating. Covers "move to a new
      router remotely": switch to a phone hotspot (must be 5 GHz), fix the primary,
      hotspot off → back to primary by itself. UI editing + "switch now with
      60 s rollback" in Phase 2; UI-edited networks (app NVS) override `prov`
- [x] **Fast reconnect** (measured, see below): `WIFI_FAST_SCAN` from the last
      channel (stored per network in NVS, written only on change), and after an
      auth timeout the retry is pinned to that BSSID immediately
- [x] Wi-Fi watchdog: one task, 1 s tick, downtime measured from the first
      disconnect (never re-armed by retries). Escalation:
  1. reconnect with backoff 1 → 30 s
  2. driver restart every 5 min of downtime
  3. after 3 driver restarts: **reboot at 20 / 30 / 60 min** of downtime (consecutive
     watchdog reboots in one outage, counted in RTC memory), **only when the
     reboot gate says it's safe** (`wifi5g_set_reboot_gate`, wired up by later phases):
     - no `timer`/`cron` due within the next 2 min, no script mid-run
     - no permit-join, interview, OTA, or backup in progress
     - registry, script `state`, and pending timer deadlines flushed
       (timers saved to RTC memory and restored after the soft reset)
     - if not safe, re-checked every second until it is
  4. so a router that's off overnight costs a handful of reboots, never a loop
  - [x] watchdog reboot logged on the next boot; [x] shown in the UI (Status → Wi-Fi recovery)
  - [~] gate implemented 2026-11-06 (`reboot_gate` in `main.c`): defers while the pairing
    window is open, a device is actively being interviewed, or a firmware upload is
    being written; flushes the device registry before allowing it. Since Phase 5
    (`automation_reboot_blocker`) also: a script running, a timer or cron job due within
    2 min, a buzzer pattern playing; script `state` and named timers saved first (timers
    with a global function name survive in RTC memory; anonymous ones are lost). Not yet
    observed deferring (needs a 20+ min outage)
  - [ ] **not yet observed firing** — needs the AP gone for 5+ / 20+ min
  - the cost being guarded: a reboot takes a few seconds until Zigbee is back
    (network restored from `zb_storage`, no re-pairing), and reports sent in that
    window are lost. Hence the gating
- [x] Log channel + RSSI + band on connect; `HUB_READY ip=… net=… ch=… rssi=… boot_ms=…`
      line for tools
- [x] mDNS `zigbee-hub.local` (espressif/mdns 1.15.0) — resolves from the dev machine
- [x] Verified on the home network **as-is, DFS**: see measurements. 22 min serial soak: 0 disconnects.
      **`hublog` soak 2026-11-04 21:18 → 2026-11-06 15:43 UTC (42.4 h, 508 samples, no gaps):
      0 disconnects, 0 NO_AP_FOUND, 0 failovers, 0 driver restarts, 0 watchdog reboots,
      1 connect.** Router stayed on ch 112 (DFS) throughout. Heap flat at 132 KB free /
      97 KB min (the min is from boot), PSRAM flat — no leak. No WARN/ERROR after boot.
      DFS stays; no need to move the router
- [x] **Bug: RSSI was stale** — read once at connect and never refreshed, so the UI,
      status API and `sample` lines showed the association-time value (−20 dBm for
      42 h). Fixed 2026-11-06: the 1 s tick refreshes RSSI (`esp_wifi_sta_get_rssi`,
      last beacon) and the channel (follows an AP channel switch). Verified: −22/−23
      alternating in the status API

#### Phase 1 measurements (2026-11-04, home network, WPA3-SAE, RSSI −30…−43 dBm)

**DFS works so far.** During the session the router moved itself from channel 52
to **112** (also DFS — radar or auto-channel); the board followed without help.

Reboot → IP (abrupt RTS reset, 4–6 boots each, identical every time). First four
rows are app time (`boot_ms`, excludes the bootloader + PSRAM init); the last two
are host wall-clock from releasing reset to the `HUB_READY` line:

| Change | Time | Where the time went |
|---|---|---|
| all-channel scan | 23.3 s app | AP heard at 2.9 s, scan continued to 9.9 s; auth timeout 4 s; retry scan another 7 s |
| + fast scan & channel hint | 15.1 s app | auth from 2.6 s; after the timeout the driver **blacklists the AP**, retry walks every channel (DFS passively, 360 ms each) |
| + BSSID-pinned retry | 8.3 s app | `set_config` with a BSSID clears the blacklist; retry connects in 0.7 s |
| + no backoff on that retry | 6.9 s app | 4 s stale-SAE timeout, ~1 s DHCP ARP check |
| + PSRAM memtest off, DHCP lease restore, no DHCP ARP check | 5.9 s app | memtest −1 s (only visible in wall-clock), ARP check −1 s |
| + **early auth abort** (2 s after connect) + pinned retry | **4.07 s wall** | associated 4.09 s, IP 16 ms later |

The stale association: after an abrupt reset (crash, RTS, short power blip) the
router still holds our old WPA3/PMF association and ignores the first auth; the
driver would wait its full 4 s. The router accepts a retry ~1.8 s after the first
auth started. A manual abort *also* blacklists the AP, so the retry must be
pinned to the BSSID. Healthy connects take 1.34 s from connect() — the 2 s abort
leaves margin. First boot after a long idle (no stale association): associated
2.05 s after boot.

What's left of 4.07 s: ~0.5 s bootloader + early init, ~0.2 s to `wifi5g_start`,
~0.6 s radio start (PHY init), 1.8 s waiting for the router, ~0.5 s auth + assoc.

Ping from the dev machine: 80/80 over 7 min, avg 9.6 ms, max 96 ms. 0 disconnects.

#### Compared with the reference repo (same chip; numbers from its README/CLAUDE.md)

| | reference (zigbee2esphome) | ours, Phase 1 |
|---|---|---|
| boot → Wi-Fi | 25–27 s, "0 failed attempts" | **4.07 s** after abrupt reset; ~2.4 s clean |
| scanning | all-channel (their own test: longer passive dwell made it *worse*, 51 s vs 27 s) | fast scan + per-network channel hint + BSSID-pinned retries |
| DFS | ch 64: API login 10–28 s, frequent timeouts | ch 52 → router moved to 112; ping 7 ms avg; `GET /api/status` p50 19 ms, max 27 ms (60 req, after 42 h up); login 0.92 s (all PBKDF2) |
| AP loss (reason 201) | periodic, at −50 dBm, recovered by watchdog | **none in 42.4 h** (no Zigbee/coex yet) |
| 2.4 GHz | falls back to AUTO band after failures | never |

Not yet a fair fight: the reference measured with **Zigbee + coex running** (and its
biggest finding is that Wi-Fi barely associates until `esp_coex_wifi_i154_enable()`
runs). We had no 802.15.4 yet. Re-run with Zigbee up (Phase 3, 2026-11-06):

| | reference (zigbee2esphome) | ours, Phase 3 (Zigbee + coex; ch 112 DFS, then ch 36) |
|---|---|---|
| boot → Wi-Fi | 25–27 s | **1.67 s** (5.9 s after an abrupt reset with a stale association); was 17.7 s until the coex-boost bug was fixed |
| Zigbee up | after Wi-Fi in their boot order | network restored in 13 ms, **before** Wi-Fi starts (formed in 576 ms on first boot) |
| reports lost under Wi-Fi load | not measured | 0 of 32 (14 idle + 18 loaded) |
| ping while associated | not reported | 19 ms avg, no power save (was ~60 ms with modem sleep, which turned out unnecessary) vs 7–10 ms without Zigbee |

### Phase 1b — Buzzer & GPIO hardware validation (user request; done 2026-11-07)
The user's buzzers: **RUNCCI-YUN "DC 3V active piezo, continuous sound"**, 20 pcs
(Amazon B0FMXS7H4N; specs hidden behind the cart, no datasheet found). "Piezo"
in such listings is often wrong — they may be 12 mm electromagnetic types
(~20–30 mA at 3 V) rather than true piezo (a few mA). **No other parts available**
(no transistors, resistors, wires, multimeter), so the board measures itself.

Independent of the other phases; its results feed Phase 5's `gpio.*` API.

- [x] **Inputs from the user** (photos, 2026-11-07):
  - buzzer: 12 mm black sealed cylinder, ~9–10 mm tall, "+" on the seal sticker, two
    legs ~7.6 mm apart by eye (= 3 header holes). The usual "12 mm active buzzer"
    package, typically electromagnetic (~20–30 mA at 3 V) despite "piezo" in the
    listing; the probe measures it instead of guessing
  - board: **Waveshare ESP32-C5-WIFI6-KIT-NXRX** (ESP32-C5-WROOM-1, N16R8). Headers:
    top `G TX RX 24 23 N/15 27 4 5 NC 28 G 14 13 G VBAT`, bottom
    `3V3 RST 2 3 0 1 6 7 8 9 10 26 25 5V G VBAT`. On board: RGB LED on IO27, BOOT on
    IO28, two USB-C ports ("UART" = CH343, "USB" = native USB on GPIO13/14)
- [~] **Pin choice** (ESP-IDF's C5 GPIO table, not memory): strapping 2, 7, 25, 27, 28;
      USB-JTAG 13, 14; UART0 11/12 (TX/RX); flash/PSRAM 16–22 (not on the header);
      32 kHz crystal pads 0/1 (`io_mux_reg.h`: `PAD_XTAL_32K_P/N`); ADC only on GPIO1–6.
      Usable on the header: 3, 4, 5, 6, 8, 9, 10, 15, 23, 24, 26. With GND 3 holes away (legs at 7.62 mm): IO24, **IO5**, IO26 — and only
      **IO5 is ADC-capable** (ADC1_CH4). Candidate: **IO5 + the GND three holes to its
      right on the top row** (5 · NC · 28 · G). Reset state: the datasheet's pad default
      table isn't in the repo; an internal pull-up (~45 kΩ, < 0.1 mA) can't sound the
      buzzer, so the check that matters is by ear: no chirp across several resets and
      boots with the buzzer plugged in. GPIO5 is the JTAG MTDO pad by default (not
      driven while JTAG is unused). To confirm: the legs fit IO5/G, and the reboot test
  - the criteria:
    - not flash/PSRAM, USB D−/D+, UART0 (GPIO 11/12), 32 kHz crystal, or strapping pins
    - **stays low through reset and boot** — otherwise the buzzer chirps on every
      reboot (and a stuck-high pin during a crash would be a fail-safe hole)
    - ADC-capable (for the self-measurement below)
    - a GND pin at a spacing the buzzer legs can reach without wires
- [x] **Probe firmware** as a separate tiny project (`tools/gpio_probe/`), flashed
      app-only and replaced by the hub firmware afterwards (`prov`, `zb_storage`
      untouched):
  1. sanity-check the trick first, with no load: drive the pin high/low and read
     the same pad with the ADC (output driver enabled while the ADC samples). If
     the C5 can't read a driven pad, fall back to listening + short bursts only
  2. with the buzzer connected: 200 ms bursts, 1 s apart, at each drive strength
     (`GPIO_DRIVE_CAP_0..3`), reading the loaded pin voltage during each burst
  3. classify: active vs passive (continuous tone on DC = active); estimate current
     from the voltage drop and the datasheet's output characteristics (±30 %)
  4. user reports loudness at each setting
- [x] **Measured 2026-11-07** (probe in `ota_1`, hub in `ota_0` untouched; commands
      `r`/`m`/`b`/`p` over UART0):
  - method works: IO5 driven high with nothing attached reads 3355–3365 mV at drive
    strengths 0–3, 0 mV driven low
  - buzzer 1 on IO5 → GND: **active, electromagnetic** (continuous tone on DC; the
    loaded pad voltage swings between ~0.4 and 3.35 V at random sample phases = coil
    switched at audio rate). Average loaded voltage at drive 0/1/2/3: 2.03/2.37/2.70/
    2.95 V (sticker on), 3.00 V at drive 3 with the sticker off (lowest sample 2.42 V).
    Estimated against the ESP family figure (40 mA at VOH 2.64 V, drive 3): ~20 mA
    average, ~50 mA short peaks (±30 %). Loudness rises with drive strength; one beep
    is useless as an alarm, a 10 s pattern (5 × [1 s of 50/50 ms stutter + 0.4 s on,
    0.6 s off]) at drive 3, sticker off, was "much better" (user)
  - **driving it directly resets the chip**: `rst:0x19 RTC_PWR_GLITCH` /
    `esp_reset_reason()` 14, 4 times in ~20 drives at drive 2–3 (never at 0), and once
    with buzzer 2 (IO8 high, IO6 low, drive 2) within 0.3 s. The C5 bootloader always
    arms the power-glitch reset on **VDDPST**, the I/O pad supply
    (`bootloader_esp32c5.c:96` → `bootloader_power_glitch_reset_config(true)`, no
    Kconfig): the coil's current pulses and turn-off spikes (no flyback diode) dip the
    pad supply. Disabling it needs a custom bootloader (USB only) and would only hide
    a real supply dip
  - buzzer 2 on IO6(+)/IO8(−): no load at first (IO6 stays at 3.36 V driven) — it
    wasn't seated properly (user). The reset with it reversed happened in that state
  - **reseated firmly, both buzzers at drive 3: 10 runs of 10 s, no reset** (user, 2026-11-07,
    observed by ear: every run played to the end). So the resets most likely came from
    loose contacts: an inductive load making and breaking contact throws much larger
    spikes than one switched cleanly. **Confirmed with serial capture**: the same 10 runs
    (50 rounds of the pattern below, back to back, ~103 s), 50/50 rounds completed,
    **0 resets**. Buzzer 1 (bent legs) had reset 4 times alone earlier — its seating
    then is unknown
  - the alarm the user picked ("perfect"), both buzzers, drive 3, 5 rounds of 2 s:
    ping-pong A/B 40 ms each × 10 (0.8 s) → both 30 ms on / 30 ms off × 10 (0.6 s) →
    both on 0.5 s (the two units' slightly different pitches beat) → 0.1 s off
  - **consequence for the hub**: direct drive at drive strength 3 works for these
    buzzers, but they must be soldered or firmly fixed (a loose one can reset the
    chip, taking Zigbee down). The hub logs the reset reason at boot, so a
    `RTC_PWR_GLITCH` reset shows up in `hublog`
- [x] **Decision rule** (recorded here with the numbers):
  - loaded voltage ≥ 3.0 V → direct drive OK, any drive strength that's loud enough
  - 2.5–3.0 V → direct drive OK at max drive strength, beeps only (no long tones),
    duty-cycle limited in `gpio.beep`
  - < 2.5 V → too much current for a pin: needs a transistor (BC547/2N2222 + 1 kΩ)
  - applied (2026-11-07): 3.00 V average at drive 3, sticker off → direct drive, drive 3.
    The peaks (dips to 2.42 V) were the open question; 50/50 clean rounds settled it
- [x] Result became Phase 5's defaults (`auto_gpio.c`): allowed pin list, drive strength, and any
      on-time/duty limits `gpio.beep` enforces for this buzzer type. From the
      measurements: pins 3, 4, 5, 6, 8, 9, 10, 15, 23, 24, 26; drive strength 3 for
      outputs; a second GPIO driven low works as a buzzer's ground (IO6 + IO8). Tested
      continuous on-times up to 1 s and the 10 s pattern above, nothing longer — so the
      first `gpio.beep` cap is 2 s per pulse (the probe's limit) until longer tones are
      measured

### Phase 2 — Web UI foundation (done 2026-11-04, verified from the dev machine)
Components: `web` (server, auth, API, UI in `web/ui/`), `hubsys` (planned reboot,
OTA bookkeeping), `wifi5g` (runtime network API).

- [x] `esp_http_server`, UI gzipped at build time (`gzip -9 -n` in CMake →
      `target_add_binary_data`), served with `Content-Encoding: gzip`, ETag = build
      id → 304 on revalidate. CSP on the page (scripts self only). **UI = 7.0 KB gzipped**
- [x] Login: PBKDF2-SHA256 (PSA `PSA_ALG_PBKDF2_HMAC`, 10k iters) → **0.96 s per
      login** on the C5. 3 free attempts, then 2…60 s lockouts. Session = 256-bit
      random token in an HttpOnly SameSite=Strict cookie; only its SHA-256 is stored,
      in NVS (8 slots, LRU), so reboots/OTA don't log anyone out
- [x] Anti-CSRF: every POST needs an `X-Hub` header (cross-site pages can't send
      custom headers without a CORS preflight, which we never answer). Verified:
      no header → 403, no cookie → 401
- [x] WebSocket `/ws`: status pushed every 2 s, sends queued with
      `httpd_queue_work` so only the httpd task touches sockets. **Auth runs in
      `ws_pre_handshake_cb`** (see findings). Verified: no cookie → refused at the
      handshake; with cookie → pushes at 2.0 / 4.0 / 6.0 s
- [x] System page: Wi-Fi (network, channel + DFS badge, RSSI, IP, MAC, time to IP),
      recovery counters, memory bars, uptime, reset + planned-reboot reason, firmware
- [x] **Wi-Fi page**: primary + backup editable; empty password = keep the stored
      one (passwords never leave the board). Switch / save / "reset to flash.sh
      networks" all on a 60 s trial; only success writes NVS. Verified: wrong
      password → offline 60 s → reverted + reconnected; correct one → OK in < 2 s,
      saved; reset → back to `prov`
- [x] Vanilla JS, no framework, no build step

#### Remote firmware updates (OTA)
- [x] `POST /api/ota`: streams into the inactive slot in 4 KB chunks (PSRAM
      buffer); checks image magic, chip id (C5) and project name *before*
      writing; `esp_ota_end` validates the whole image
- [x] UI: Firmware page with upload progress; after the reboot it polls and
      reports "running the new firmware" / "rolled back"
- [x] Bookkeeping in NVS (`ota`: pending build id, from id) → on the next boot
      `updated` or `rolled_back`, shown in the UI and the status API
- [x] **Confirmation rule**: Phase 3 version — confirmed once Zigbee is up and the web
      server runs, held 5 s (Wi-Fi not required); not healthy within 120 s →
      `esp_restart()` → bootloader rolls back. Verified 2026-11-06 over the air:
      confirmed in 26.6 s and 35.2 s total
- [x] Planned reboots disconnect Wi-Fi cleanly first (`wifi5g_prepare_reboot`), so
      the next boot doesn't hit the stale-association login delay
- Updates restart into the new firmware immediately, never gated (user decision
  2026-11-06: an update is intentional). Only the Wi-Fi watchdog's reboot is gated (Phase 1)
- [x] `flash.sh --ota [--host H]` (default `<hostname>.local`, falling back to the
      MagicDNS name `<hostname>` when that doesn't answer — tailnet-only access): build → login (password via stdin) → upload → wait →
      report. **Measured: upload 5.6 s, confirmed 16 s after upload start, ~21 s
      for the whole command when nothing needs rebuilding.** Faster than USB
- [x] Rollback demonstrated 2026-11-06: an image that calls `abort()` at the start of
      `app_main`, installed with `flash.sh --ota` → crashed on its first boot → the
      bootloader loaded the previous slot (`ota_0`), the hub logged `ROLLED_BACK` and the
      core-dump summary, Wi-Fi back in 2.5 s, `flash.sh` reported the rollback
- [x] What OTA can't change: bootloader, partition table — USB only, by design
- [ ] **Optional: signed updates** (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` +
      `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`, RSA-3072, Secure Boot V2
      signature format, no eFuses burned):
  - verification runs in the **running app** when an update arrives — no
    bootloader change, so it can be switched on later through a normal OTA
    (the old firmware doesn't check, the new one does from then on)
  - `flash.sh --gen-signing-key` creates the key in `.esp/secrets/` (gitignored);
    builds sign automatically (`CONFIG_SECURE_BOOT_SIGNING_KEY`)
  - effect: even with the admin password, nobody can install firmware you didn't sign
  - **the key must be backed up**: lose it and every future update needs USB.
    `flash.sh` refuses to enable signing until the user confirms a backup exists
  - USB flashing keeps working regardless (this isn't hardware secure boot)

#### On-board diagnostic log (`hublog`, user request; stays in the final build)
For long tests without a serial cable: log on the board, download later.
- [x] `/storage` = LittleFS (joltwallet/littlefs 1.23.1) on the `storage`
      partition; first mount formats it (one-time "Corrupted dir pair" error is
      just the blank partition). Mount 5 ms
- [x] `esp_log_set_vprintf` hook keeps WARN/ERROR from everything + INFO from
      `wifi5g hubsys web* sample boot hublog prov`; strips colour codes; drops
      prefix-only fragments (the Wi-Fi driver prints capability dumps in pieces)
- [x] The hook only copies into a 32 KB PSRAM ring (it can run inside the Wi-Fi
      driver: no flash I/O there); a task writes to flash every 10 s, and a
      shutdown handler flushes on every `esp_restart()` (watchdog, OTA, planned).
      A hard crash loses ≤ 10 s
- [x] Two rotating files, 256 KB each (`/storage/logs/events{,.1}.log`)
- [x] UTC timestamps via SNTP (`pool.ntp.org`, synced 3 s after IP); the RTC keeps
      time across soft reboots; `+Ns` uptime before the first sync after power-on
- [x] `sample` line every 5 min: uptime, Wi-Fi state, channel, RSSI, counters, heap
- [x] `GET /api/logs` (authenticated, flushes first, streams both files) +
      "Download log" on the System page
- Cost: 64 KB PSRAM, no internal RAM; flash writes only when there are new lines
  (~50–100 KB/night), wear-levelled over 7 MB
- Phase 5 builds the script log (`log(...)`, script errors) on this

#### Phase 2 findings
- **IDF 6.1 never calls the WebSocket URI handler for the handshake.** The
  common pattern `if (req->method == HTTP_GET) { /* auth */ }` is dead code: an
  unauthenticated client got a socket. Auth must be in `ws_pre_handshake_cb`
  (`CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT`), registration in the post hook
- **Don't drop the link right after replying.** Switching Wi-Fi 30 ms after the
  `202` lost the reply in flight; the client received it only when the board came
  back (60 s later on a failed trial) — the UI would have hung. Switches now start
  ≥ 800 ms after the request is accepted
- **Status must not show a stale result.** The trial state is set to `running`
  when the request is accepted, not when the Wi-Fi task picks it up
- **`flash.sh` waiting on serial**: `cat | tr | sed | awk` held the ready line for
  minutes (`tr` block-buffers into a pipe; `cat` only died on its next write after
  awk exited, ~30 s). awk now reads the port directly: `--prov-only` 42 s → 15 s
- Internal heap with web server + a logged-in WebSocket client: ~139 KB free
  (min ~111 KB)

### Phase 3 — Zigbee coordinator + enrollment (done 2026-11-06; availability tracking not yet observed firing)
- [x] `espressif/esp-zigbee-lib` **2.1.0** (released 2026-10-30, built against IDF
      5.5.6) builds and links on IDF 6.1. Stack NVRAM in the NVS `zb_storage` partition
- [x] **Coex before Wi-Fi connects**: `esp_coex_wifi_i154_enable()` in `zb_start()`,
      before `wifi5g_start()`
- [x] **Coex priority — measured 2026-11-06** (802.15.4 PTI via `esp_ieee802154_set_coex_config`):
  - txrx/txrx_at HIGH permanently: Wi-Fi never associates (NO_AP_FOUND after 15 s,
    every attempt). A coordinator is always receiving, so HIGH never yields
  - driver defaults (idle 4, txrx LOW, txrx_at MIDDLE) from boot: network formation
    never completes, with Wi-Fi `PS_NONE` or `PS_MIN_MODEM`
  - **chosen**: HIGH until the network is formed/restored, defaults after.
    Formation 576 ms, restore 8 ms (done before Wi-Fi starts)
  - Wi-Fi **without power save** (`WIFI_PS_NONE`) since 2026-11-06. Modem sleep was used
    first ("formation never completed with PS_NONE"), but that was measured with the
    boost bug below. Re-measured with PS_NONE: ping 19 ms avg (2–60; modem sleep ~60 ms),
    80/80 Door events strictly alternating under load (3927 small downloads + ping every
    50 ms, 2966/2966 answered; lighter in bytes than the modem-sleep test: 2 MB vs 29 MB),
    boot → IP unchanged. User decision: modem sleep not needed
  - **Zigbee reception with these priorities — measured 2026-11-06**, Door sensor
    opened/closed by hand (~2 s per state): Wi-Fi idle 14/14 events, Wi-Fi loaded
    (1059 downloads = 29 MB in 170 s from the hub + ping every 50 ms) 18/18 events.
    Strict open/closed alternation, longest gap 3.7 s: **no report lost**
  - **Boot → Wi-Fi was 17.7 s with Zigbee — root cause found and fixed 2026-11-06:
    our coex boost never ended.** `esp_ieee802154_set_coex_config()` only *stores* the
    PTI values; the 802.15.4 driver applies a PTI when an operation starts (`enable_rx`,
    transmit, ED — `esp_ieee802154_util.c`, `esp_ieee802154_dev.c`). The coordinator sits
    in one long receive, so the HIGH set for the restore stayed in force until its first
    transmit. Measured with a link-time `--wrap` on `esp_coex_ieee802154_txrx_pti_set`:
    PTI HIGH at 0.85 s, LOW only at the first TX (10.9 s / 13.9 s). While 802.15.4 is HIGH,
    Wi-Fi can't transmit at all: the first connect's single probe on ch 36 (at 1.7 s) never
    reached the air (laptop mt7925 in monitor mode on ch 36: 0 of 5 boots; 0 of 30 extra
    36-only probes), so the walk went on through every channel (15 s, DFS ones passively)
    → NO_AP_FOUND, and the retry at 18.4 s connected. Rare fast boots = an early 802.15.4
    transmit. **Fix**: `coex_settle()` also calls `esp_coex_ieee802154_txrx_pti_set(txrx)`.
    Verified: PTI LOW at 0.864 s; boot → IP **1.67 s** (no stale association) and **5.9 s**
    after an abrupt reset (4 s = AP holding the stale association; 5 boots). Zigbee keeps
    the priorities the plan chose — this only ends the boost when it was meant to end
  - How it was found (so nobody repeats it): blamed on DFS passive scanning first — wrong
    (moving the router to ch 36 changed nothing); then on "coex denies Wi-Fi until the
    driver's first connect failure" — wrong (forcing an early failure at 1.8 s didn't
    help); the A/B that pointed at Zigbee: no Zigbee → probe on air at 1.74 s every boot;
    Wi-Fi started 20 s after Zigbee → connected on the first attempt. Removed earlier
    because they "made it worse under coex", but measured *with this bug*: the early auth
    abort (Phase 1). Re-measured after the fix, 5 abrupt resets: clean boots 1.66 s; with
    a stale association the AP ignored the aborted-and-retried auth too (retry at 3.8 s →
    AUTH_EXPIRE at 7.9 s, IP ~8.5 s) vs 5.9 s without the abort → stays removed. The AP
    now ignores new auths for ~4 s (Phase 1, no Zigbee: 1.8 s). Planned reboots deauth
    first (`wifi5g_prepare_reboot`), so only crashes and power blips pay the 4 s
  - Laptop capture notes: the mt7925 monitor interface works only as `wlan0` itself
    (NetworkManager told `managed no`, RF-kill unblocked), and in that mode it captured
    on ch 36 but nothing on 40 even from a hub probing without coex
- [x] Form network on first boot: channel mask {15, 25} → channel 15; random PAN
      (0x4668); network key generated by the stack. Starts **before Wi-Fi**, never waits for it
- [x] Device registry (`zb_dev.c`): PSRAM table (32 devices), persisted to
      `/storage/zb/devices.json` by a saver task (no file I/O on the Zigbee task),
      **including endpoints and cluster lists**
- [x] Interview (Basic "magic" read + Active EP in parallel on announce → Simple Desc
      → IAS CIE write + unsolicited enroll + zone read → bind + reporting; 2 s retries
      ×6, then resume on the next check-in). **Verified 2026-11-06 with two Zbeacon DS01
      contact sensors** (Tuya-made, IEEE a4c1…, ep 1: Basic, Identify, Power Config,
      IAS Zone, Poll Control; zone type 0x0015):
  - join → card in the UI within a second; Basic answered 0.7 s after the announce
  - open/closed via Zone Status Change Notifications from the first second, before
    the interview finished (they notify the coordinator without being enrolled)
  - after the fix below: interview resumed on the next wake-up and **completed in
    ~0.7 s** (EPs → descriptor → CIE write ok → zone type → reporting configured →
    the device's own Enroll Request answered)
  - bug found: ZDO callback context carried the 64-bit IEEE in a pointer (32-bit on
    the C5) → truncated → every Active EP response dropped. Context is now slot+short
  - battery 100 % / 3.0 V, RSSI −44…−66 dBm at a few metres
- [x] Web: Devices page (default), `GET /api/zb`, `POST /api/zb/{permit,rename,remove}`;
      live over WebSocket (polls only when the socket is down). Permit-join round trip verified (open 10 s → 9 s left → closed)
- [x] OTA health = Zigbee up + web server (Wi-Fi no longer required)
- Internal heap with Zigbee + Wi-Fi + web (no client): **95 KB free, lowest 63 KB**
  (Phase 2: 133 / 98) — Zigbee costs ~40 KB
- [x] Enrollment ("Scan for devices" in the UI):
  - [x] permit-join 120 s (button toggles open/close) with live countdown over WebSocket
  - [x] devices appear live as they announce (WebSocket push within 100 ms)
  - [x] interview: active endpoints, simple descriptors, Basic cluster (manufacturer,
    model, power source); the card shows "interviewing" until done
  - [x] bind + configure reporting (battery, temperature, humidity, on/off, occupancy)
    (reference bug: never initialised → no reports ever arrived)
  - [x] user names the device; persisted to LittleFS **including the cluster list**
    (reference bug: restored devices had none)
  - rooms: not doing (user decision 2026-11-06)
- [x] **Acceptance test: a cheap no-name reed/contact sensor** — passed 2026-11-06 (above) (typically Tuya
      TS0203 or similar). Press "Scan", insert the battery, it shows up, gets
      interviewed, and reports open/closed. What that needs, because these devices
      are sleepy end devices that give you seconds, not minutes:
  - interview starts **immediately** on `device_announce`, no queueing behind
    other work; all reads batched; retries short and fast while the device is
    still awake after joining (it polls its parent briefly, then sleeps)
  - **Tuya "magic packet"**: read Basic attributes (manufacturer name, ZCL
    version, app version, model ID, power source, 0xFFFE) in one request right
    after join — many Tuya devices stay silent or misbehave without it
  - **IAS Zone enrollment**: write our IEEE to `IAS_CIE_Address`, answer the
    `Zone Enroll Request` with a `Zone Enroll Response` (and also send an
    unsolicited enroll response, some devices never ask). Without this a contact
    sensor joins fine and then **never reports**
  - accept `Zone Status Change Notification` as the state source (not attribute
    reports) for contact/motion/leak
  - a sensor that fell asleep mid-interview is finished on its next check-in,
    instead of being dropped as failed
  - the UI shows it within a second of the announce, even before the interview ends
- [x] Capability mapping, sensors only: IAS Zone (contact/motion/water/smoke) and Power
      Config (battery) verified on the contact sensors; Temperature, Humidity and
      Occupancy reporting built (no such sensor here to verify). Pressure and
      illuminance: not doing (user decision 2026-11-06)
- [x] Device page: live state, LQI/RSSI, last seen, availability, rename, remove — done;
      re-interview (redoes every step; battery devices at their next check-in) — verified
      2026-11-06 on the Door sensor: pressed twice, each time "waiting for it to wake up",
      then on the next open/close the full interview ran and completed in 620 / 639 ms.
      **No controls in the UI (user decision 2026-11-06)**; since 2026-11-07 nothing
      commands devices at all (no `zb.set` in Lua, no commands from Home Assistant)
- [~] Availability tracking — built: battery devices offline after 25 h of silence.
      **Not yet observed firing** (needs a sensor silent for 25 h)
- [x] Network map from the coordinator's neighbour table (both sensors listed with
      LQI/RSSI/depth)
- **Zigbee routers (mains devices: plugs, bulbs) are not tested, and this hub is not
  built for them** (user decision 2026-11-06)
- [x] **Web server stalls (found and fixed 2026-11-06; re-interview re-tested by the user)**:
  - one WebSocket client that stops reading blocked the server's only task for
    `send_wait_timeout` (10 s) per push — measured 12 s + 7 s freezes for every page
    and login. Now WS sockets get `SO_SNDTIMEO` 0.5 s and a failed push closes that
    client (browser reconnects). Re-measured: one 1.6 s blip (mostly the test
    client's own 0.9 s login), then 11–32 ms
  - `zb_network_json` took the Zigbee lock on the HTTP task (up to 200 ms per
    snapshot); during an interview, every state change queued another snapshot and
    the server fell behind ("ctrl socket queue full"). Now the Zigbee task refreshes
    a copy of PAN/channel/neighbours every second and the HTTP task only copies it
  - the Devices page didn't redraw while anything in the cards had focus, so the
    clicked Re-interview button froze the cards. Now only a focused text field pauses
    it; Re-interview shows requested → re-interviewing → done
- UI: three tabs since 2026-11-06 — Devices, Status (read-only: Wi-Fi, recovery
  counters with the last disconnect reason in words, memory, system, firmware),
  Settings (everything that changes something: Wi-Fi networks, firmware update,
  log download, restart)
- [x] Zigbee lock around every stack call from other tasks — audited 2026-11-06, none
      missing. All `ezb_*` calls in `zb_dev.c` (interview, enroll, bind/report,
      availability pings) are reached only from the Zigbee task (signal/ZCL/ZDO callbacks,
      `zb_dev_tick` posted via `esp_zigbee_task_queue_post`). From the HTTP task:
      `zb_permit_join` and `zb_network_json` take `esp_zigbee_lock_acquire`; `zb_remove`
      posts the Mgmt_Leave to the Zigbee task; rename/list/lookup touch only the registry
      (`s_mtx`). Lock order is always Zigbee lock → `s_mtx` (web never nests them), and the
      saver copies under `s_mtx` and writes the file after releasing it.
      A failed registry save sets `s_dirty` again and is retried (fixed 2026-11-06, `e0997a7`);
      since 2026-11-07 the retries back off (4 s doubling to 60 s) and log every 10th failure

### Phase 4 — Tailscale (done 2026-11-07)
- [x] **Spike**: microlink builds and runs on IDF v6.1 / mbedTLS 4: ChaCha20-Poly1305
      through PSA (the chachapoly module is private in mbedTLS 4), randomness from
      PSA (no entropy/CTR-DRBG contexts), DERP TLS verified against the IDF CA bundle
      (upstream used `VERIFY_NONE`), failed DERP connects free their TLS context
      (upstream leaked ~20 KB per attempt)
- [x] microlink + wireguard-lwip in `components/tailscale/` with their licences
      (`microlink/LICENSE`, `microlink/x25519-license.txt`, `wireguard/LICENSE`).
      Cellular/PPP/AT-socket transport gone; the last dead `ml_at_socket_is_ready()`
      branches and the unbuilt `ml_peer_nvs.c` removed 2026-11-07
- [x] We own Wi-Fi: `ts_task` starts microlink once the STA has an IP; microlink has
      no Wi-Fi calls left (the optional TX-power setter was removed 2026-11-07)
- [x] Memory: H2/JSON buffers and all four microlink task stacks (42 KB) in PSRAM;
      mbedTLS buffers in PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`, lever 3). Audited:
      no PSRAM-stack task writes flash (node keys and settings are written by
      `ts_task`, internal stack). With Zigbee + Wi-Fi + web + Tailscale (no UI
      client), after IPv6 was turned off: **79 KB free, lowest 42 KB** (2026-11-07)
- [x] Settings in our UI: auth key, on/off, log out, access mode. The hostname comes
      from `flash.sh --hostname` (prov). No peer allowlist, on purpose: who can reach
      the hub is decided by Tailscale ACLs
- [x] UI: state, tailnet IP, MagicDNS name, home relay region, peers with
      offline / direct / "relay (DERP fra)" (Status page)
- [x] Verified: `tailscale ping zigbee-hub` from the dev machine (direct, 4–5 ms);
      from peer A (another house, same ISP) direct via the NAT-PMP endpoint, 10–27 ms,
      15/15. UI from a phone on cellular: works (user, 2026-11-07)

#### What the port needed (measured 2026-11-06/07)
- **Online state**: microlink's "online" meant "has a slot in my table"; it now uses
  the control server's `Online` and live `OnlineChange` updates (parsed in the
  wrong format upstream, so they never matched)
- **DISCO key cache**: every DISCO message did a fresh X25519 (tens of ms);
  now once per peer. `tailscale ping` 85 → 62 ms
- **Logging cost**: ~10 INFO lines/s of per-packet DISCO/DERP logs, each ~7 ms on the
  115200-baud console, inside the tasks that move packets. Demoted to DEBUG:
  `tailscale ping` 50 → **4 ms**. `POST /api/ts/debug` turns them back on for 10 min
- **Home DERP region was hard-coded to Dallas** (`ML_DERP_REGION` 9, also the STUN
  server): every relayed packet crossed the Atlantic, and a DERP server only
  delivers to clients connected to it, so peers homed elsewhere (peer A on fra,
  peer B, a cloud VM, on iad) got `PeerGone` for every DISCO ping and CallMeMaybe and never
  answered. Now: home region by STUN latency to every region (3 rounds, minimum per
  region — the minimum filters the coex sleeps), reported as `PreferredDERP`:
  **fra, 19–22 ms**. Plus up to 2 extra DERP connections to peers' home regions,
  opened on demand, closed after 60 s without our traffic, 60 s back-off for a
  region that fails. peer B relay → direct
- **DERP Nagle**: each frame was two TLS records (header, payload) and Nagle held the
  second for the server's delayed ACK. One record per frame + `TCP_NODELAY`:
  peer A → hub via DERP 58–199 ms → 41–49 ms (plus the coex sleep, below)
- **Pongs** went out over DERP even for direct pings, keeping other-region DERP
  connections alive for nothing; now only when the ping came via DERP
- **LAN vs hairpin**: a LAN peer answers on its LAN address and via the router's
  public address (through the ISP's CGNAT, 90–280 ms); whichever pong came first won.
  A trusted LAN path is no longer replaced by a public one
- **CallMeMaybe** was sent once, at startup (before DERP was even connected); now with
  every relay probe, and a direct ping from a relayed peer is answered with a burst
  of pings back (0, 0.2, 0.5, 1, 2, 3.5 s) to the address it came from
- **Probing follows need** (user request; upstream: 3 s heartbeats and 15 s probes
  for every peer, forever):
  - active peer (WireGuard data, not keepalives, within 45 s): direct → heartbeat 3 s;
    relay → probe + CallMeMaybe every 5 s
  - idle, UI open (a WebSocket client connected): direct → heartbeat 10 s (inside the
    15 s trust); relay → probes taper 15 → 30 → 45 s. Opening the UI refreshes every
    path at once (measured: all direct again within 2 s)
  - idle, no UI: nothing. Traffic starting restarts discovery at once
  - offline (control server): never. The control server pushes their return live
- **No persistent WireGuard keepalive** (was 25 s for every peer with a session,
  forever); Tailscale peers don't use one either
- **NAT-PMP** (`ml_pmp.c`): see "A peer behind the same ISP" below
- **IPv6 off** (`CONFIG_LWIP_IPV6=n`): nothing used it; cost 43.6 KB flash and 2.0 KB
  static internal RAM. microlink's unused IPv6 STUN code is behind `LWIP_IPV6`

#### Latency over the tunnel: the coex sleep (measured 2026-11-07)
- Despite `WIFI_PS_NONE`, the hub sends QoS-Null PM=1 / PM=0 once per beacon (laptop
  mt7925 in monitor mode, 25 s, 244 of each for 243 beacons): **Wi-Fi sleeps 56 ms
  (54.6–58.2) of every 102.4 ms** while 802.15.4 has the radio: PM=1 at ~43 ms after
  the beacon, PM=0 ~3 ms before the next. The AP holds frames for the hub meanwhile
  (delayed ping replies arrive on exact multiples of 102.4 ms). It's the coex
  scheduler (closed `libcoexist`; the IDF docs describe 802.15.4 coex as priority
  only). Not changed: it is "Zigbee wins"
- Effect: ~53 % of packets to the hub wait 0–56 ms (~15 ms average) per direction,
  LAN and tunnel alike
- Tunnel cost on the hub: 5.4 ms turnaround on air vs 1.8 ms LAN (+3.6 ms per packet)
- `tailscale ping` showed a clean 48 / 100 ms alternation via DERP: it sends the next
  ping 1 s after the previous reply, so the RTT shifts the next ping's phase in the
  102.4 ms cycle by ±26 ms — in, out, in. An artifact of the tool, not of the hub
  (hub-internal DERP ping → pong measured flat at 4–7 ms)

#### The "post-boot stall" was the dev machine (measured 2026-11-07)
- Symptom: right after an OTA or restart, the UI and pings from the dev machine
  stalled for 3–21 s at a time (logins of 4.6–9.8 s, TCP SYNs unanswered for 5 s,
  up to 202 pings in a row lost), while the router and the phone (another Wi-Fi
  client on the same AP) lost nothing in the same windows
- Ruled out on the hub, each measured: lwIP core lock (longest hold 102 ms, sampler of
  the holder every 50 ms), socket exhaustion (peak 9 of 24), CPU stalls (a priority-15
  task never woke > 300 ms late), coex (802.15.4 back to default priorities 1.5 s
  after boot)
- Cause: **ARP flux on the dev machine**. It has Ethernet and Wi-Fi on
  the same subnet with Linux's default `arp_ignore=0`, so both interfaces answer the
  hub's "who-has <its Ethernet IP>" — Wi-Fi 68 ms after Ethernet — and lwIP keeps the last
  answer. From then on the hub sends everything for that address to the laptop's Wi-Fi card
  (power save on): 248 echo replies seen arriving on wlan0, outages until the entry
  changes. Only after a boot (empty ARP table); never with the laptop's Wi-Fi off the
  network (monitor mode: 0 outages in ~6 min, an OTA included)
- Not a hub bug. Fix on the dev machine if it matters: `arp_ignore=1` + `arp_announce=2`,
  or not both interfaces on the same subnet
- Diagnostics added while hunting it, kept: `lwIP core lock held N ms by <call>` (WARN,
  ≥ 200 ms; normal holds are 50–100 ms) for every WireGuard call, a 50 ms sampler of the core-lock holder, a socket
  census (`lwIP sockets in use: N of 24`, full list at ≥ 21), and a CPU-stall detector

#### A peer behind the same ISP: why IPv4 direct needs NAT-PMP (measured 2026-11-07)
- Peer A (a container in another house, same ISP) never got a direct path. Sniffed on
  peer A: its pings reached the hub; not one IPv4 packet from our network reached it
- The ISP routes between its own customers internally: packets to peer A's public
  <peer A's public IPv4> never reach the CGNAT (which sits at the ISP's exit; traceroutes show
  only private hops to peer A's router, a public hop only toward 1.1.1.1). They
  leave with our router's ISP-side address <router WAN 10.x> and whatever port the
  router picked. peer A's router filters by exact address:port (shown with peer B:
  replies from the port it sent to pass, another port of the same host is dropped),
  so they were dropped
- tailscaled on the dev machine asks the router for a NAT-PMP mapping and advertises
  <router WAN 10.x>:41641 — that is why its IPv4 path "sometimes" worked (and IPv6
  otherwise). Its frequent path switching: within a discovery round the IPv4 pong
  arrives first and the IPv6 pong a moment later wins (tailscaled prefers IPv6 at
  similar latency); new rounds start whenever the path expires after a pause
- Now the hub maps UDP 51820 on the router (NAT-PMP, RFC 6886; 2 h lifetime, renewed
  at half-life, retried every 10 min if the router doesn't answer) and advertises
  <router WAN 10.x>:51820 as a "portmapped" endpoint (and in CallMeMaybe). peer A → hub
  **direct, 10–27 ms, 15/15**, from both sides (was 120–220 ms via DERP). A 20-packet
  raw test over the same path: 20/20
- With a public IPv4 from the ISP (user plans to ask), the CGNAT disappears; NAT-PMP
  stays useful (it makes our router's port predictable)

#### Access mode: "Tailnet only, no password" (user request; built 2026-11-07)
Offered in the UI (Settings → Tailscale) once Tailscale is connected. Code:
`components/web/access.c` (+ `hub_ip4_input_hook` in `components/tailscale/ts.c`).
- [x] Web UI, `/api`, `/ws` and OTA accept connections **only via the Tailscale
      interface**, and the admin password is not asked there (`web_guard`, the WS
      pre-handshake check, `/api/session`, `/api/login` all pass for tailnet
      connections in this mode). The ESPHome API (Phase 6) must use the same check
- [x] Enforcement, two layers (lwIP is a weak host):
  1. `httpd_config_t.open_fn` = `access_open_fn`: in this mode a connection whose
     local address isn't the tailnet IP or whose peer isn't in 100.64.0.0/10 is
     closed before a byte is read
  2. `LWIP_HOOK_IP4_INPUT` (custom hook header `hub_lwip_hooks.h`, set on the lwip
     component from `components/tailscale/CMakeLists.txt`): packets that arrive on
     the Wi-Fi netif addressed to the tailnet IP are dropped — **always**, not only in
     this mode (they're never legitimate). Armed once Tailscale has an IP; counter
     `forged_dropped` in `GET /api/access`
- [x] mDNS stopped in this mode (`mdns_free`), restarted when the LAN opens again
- [x] Switching on: `POST /api/access` needs the password once more, a connected
      Tailscale, and the request itself coming over Tailscale (LAN → 409)
- [x] Lockout recovery: BOOT (GPIO28, the C5's download-mode strap) held 10 s →
      LAN with password for 15 min; `flash.sh --prov-only --lan-access` writes a fresh
      nonce into `prov` (`lanreset`), a nonce not seen before turns the mode off at
      boot. USB path verified by Claude; BOOT press verified by the user (2026-11-07).
      GPIO28 reads 1 = released at boot
- [x] Who on the tailnet can reach it is decided by Tailscale ACLs (README says so)
- [x] Verified 2026-11-07:
  - forged TCP SYNs from a LAN host (scapy, Ethernet frames straight to the hub's MAC)
    to <tailnet IP>:80: 0 of 3 answered, `forged_dropped` 3; control SYNs to the LAN
    IP: 3 of 3 answered. (Not run: the same test with the hook removed)
  - switch on: from the LAN → 409; over the tailnet with a wrong password → 401;
    with the right one → on
  - in the mode: LAN port 80 reset before any response (curl exit 56); mDNS name
    gone; over the tailnet `/api/session` authed and `/api/status` 200 without a
    cookie; from peer A (another house) the same, UI page 200
  - `flash.sh --ota` with no `--host`: `.local` doesn't answer → tailnet name, login
    without password, upload 22.8 s over the tunnel, confirmed; mode kept after reboot
  - `flash.sh --prov-only --lan-access`: mode off, LAN + password and mDNS back

### Phase 5 — Automation (Lua 5.5) (built and mostly verified 2026-11-07; door trigger and router-off test pending)

**Tests still to do** (user: "tests are last"), in one place:
- `zb.on` firing on a real door change (`t_door` test script is loaded on the hub), then
  a restart: the door's last state must come back (`devices.json` persistence)
- router off for an hour: triggers, timers, cron and buzzers keep working; `net.on("down")`
  fires; a `retry_for` request made offline goes out when the router is back
- `gpio.on` with an input; buzzers audible with `gpio.pattern` (the user listens)
- UI on the phone: Automations layout (Secrets/GPIO moved there, wrapping rows), the
  CodeMirror editor (typing, completion, error line on Save), Settings → Time zone
- the Wi-Fi watchdog's reboot gate deferring for a script (needs a 20+ min outage)

Built in addition to the list below: `zb.devices()`, `timer.cancel(id)` (also cancels
cron), timers by global function name (survive a restart), `json.encode/decode`,
`print` = `log`.

**Decided (user, 2026-11-04): Lua.** Why: most pasted scripts will be written by
the user or an AI, and Lua is widely known; the C API gives per-script memory caps
(custom allocator) and runaway-loop aborts (instruction hook); ~200–250 KB flash;
MIT. Rejected: Berry (Tasmota; lean but niche), QuickJS (full JS, viable but
heavier), mJS/Elk/Espruino (JS subsets — pasted normal JS often fails). ESPHome
has no runtime scripting (YAML compiled to C++), so nothing to borrow there.

- [x] Vendor Lua 5.5.1 (latest at time of writing; MIT) into `components/lua/`; allocator into PSRAM with a
      per-script memory cap (default 256 KB). Unmodified sources; `LUAI_MAXCCALLS=100`
- [x] One `lua_State` per script, all run on one automation task (events are
      queued to it — Lua never runs on the Zigbee or HTTP task)
- [x] Sandbox: no `io`, `os.execute`, `require`, `load` of bytecode; instruction
      count hook aborts runaway scripts; errors reported to the UI with line numbers
- [~] API — verified on hardware: `timer.*`, `cron`, `state` (kept across reboots),
  `json`, `http` (HTTPS, POST + headers, LAN plain HTTP, bad host → 0), `secret` + masking,
  `gpio.*`, `net.online`/`net.on("up")`, `zb.devices`/`zb.get`. **Not yet observed:**
  `zb.on` firing (needs a door opened), `net.on("down")`, `retry_for` across an outage,
  `gpio.on`. Last known device state (zone status, on/off, occupancy within 2 s of a
  change; battery/temperature/humidity at most every 5 min) is saved in `devices.json` and
  restored at boot, so `zb.get` and the UI have it right after a restart (built
  2026-11-07, verification pending: needs a sensor report, then a restart). Side effect:
  the first report after a boot no longer counts as a change unless the value differs
  - `zb.on(device, attr, fn)` — state change triggers
  - `zb.get(device, attr)`. No `zb.set` (user decision 2026-11-07): the hub reads its
    sensors, it doesn't command devices
  - `timer.after(s, fn)`, `timer.every(s, fn)`, `cron("0 7 * * 1-5", fn)`
  - `state.get/put(key, value)` — small persistent KV for scripts
  - **HTTP requests** (user request: call an Alexa skill URL that sounds the
    alarm, instead of or together with the buzzer). Typical target: a
    third-party Alexa skill / routine trigger that gives an HTTPS URL with a
    token in it; calling it runs an Alexa routine (alarm sound, announcement)
    - `http.request{url=, method="GET"|"POST"|"PUT", headers={}, body= | json={},
      timeout=10, retry_for=0, on_done=function(status, body) end}`
      plus shorthands `http.get(url, opts)` / `http.post(url, body, opts)`
    - **never blocks the script**: queued to one dedicated HTTP task, the result
      comes back later through `on_done` on the automation task (status 0 =
      network error / gave up)
    - **HTTPS** with the ESP-IDF certificate bundle (public CAs), certificates
      verified; plain HTTP only to LAN / tailnet addresses (private, link-local, 100.64/10)
    - **`retry_for = seconds`**: while offline or on errors (5xx, timeouts),
      retried with backoff until that deadline — for alarms that must arrive
      once Wi-Fi is back. Default 0 = one attempt, dropped when offline
    - limits: response bodies truncated at 16 KB; queue of 16 requests;
      per-script rate limit (default 10/min); one TLS connection at a time
      (mbedTLS buffers in PSRAM since Phase 4; no visible change in the internal
      low-water mark when a script made HTTPS calls)
  - **Secrets**: `secret("alexa_alarm_url")` reads a value stored under
    Automations → Secrets (write-only in the UI, kept in NVS, never shown in script
    text or logs). URLs with tokens belong there, not pasted into scripts;
    `log()` masks secret values if a script prints one
  - `log(...)` → UI log stream
  - **GPIO** (user request: 3 V buzzers on the board's pins):
    - `gpio.claim(pin, ...)` — top level only (2026-11-07, see GPIO safety)
    - `gpio.write(pin, 0|1)`, `gpio.read(pin)`, `gpio.on(pin, "rising"|"falling"|"both", fn)`
      (debounced, delivered through the automation queue — never from the ISR)
    - `gpio.beep(pin, ms)` / `gpio.beep(pin, {on=100, off=100, times=3})` for
      **active** buzzers (on/off), non-blocking, driven by a timer not the script
    - `gpio.tone(pin, hz, ms)` for **passive** buzzers (LEDC PWM)
    - **`gpio.pattern(pins, steps, {rounds=N})`** (user request 2026-11-07) for several
      buzzers in step: `steps` = `{{mask, ms}, …}`, bit *i* of `mask` = `pins[i]` on.
      Played by C on a precise timer (30–40 ms steps would jitter if Lua timed them),
      non-blocking; `gpio.stop(pins)` ends it. The alarm from Phase 1b is the built-in
      example. A buzzer's ground may be a second pin driven low (Automations → GPIO:
      "ground for …"). Defaults from Phase 1b: drive strength 3, 2 s cap per pulse
  - **Network state** (user request 2026-11-07): `net.online()` (Wi-Fi associated with
    an IP, now) and `net.on("up"|"down", fn)`, so a script can change what it does
    when the uplink is gone (e.g. a longer local alarm when Alexa can't be reached)
- [x] GPIO safety — verified 2026-11-07 (buzzer1 IO5, buzzer2 IO6 + ground IO8, config
      left on the hub): pins 28 and 11 refused; a second script using an owned pin stops
      with "GPIO 5 is used by script …"; a script that errored 0.3 s into a 5-round
      pattern left both pins low, the pattern stopped and no owner (checked 1 s after the error)
  - only pins enabled in the UI (Automations → GPIO: name, direction) are usable;
    scripts may refer to them by name (`gpio.write("buzzer", 1)`)
  - pins that are never allowed: SPI flash / PSRAM bus, USB D−/D+, UART0 console
    (GPIO 11/12), strapping pins, 32 kHz crystal pins. Exact list from the C5
    datasheet + this board's pinout when implementing — don't guess
  - one script owns a pin at a time. **Claims only while the script loads** (user decision
    2026-11-07): any gpio call at top level claims, `gpio.claim(...)` for pins used only in
    handlers; a handler using an unclaimed pin gets an error. So a conflict is reported at
    Save (the save response carries it: `started: false`), never when the trigger fires
  - **fail-safe**: when a script stops (disabled, error, timeout, OOM, deleted),
    every output it owned goes low and any beep/tone stops — a crashed script
    must never leave a buzzer screaming
  - the UI GPIO page shows each enabled pin's state and owning script
- [x] **Time without Wi-Fi** — the RTC keeps the time through soft resets, panics and
      watchdog reboots, so the clock is valid at once after them (seen: after the panic
      reset on 2026-11-07, cron fired at 15:25 before Wi-Fi was up). Only a power-off loses
      it; then cron waits for SNTP and the Automations page says "clock not set". **The
      last-known-time-in-NVS idea is dropped**: after a power-off it would be wrong by the
      time the hub was off, which is exactly the "fires at the wrong time" this rule forbids.
      Original text: event triggers and `timer.*` need no clock and always
      work. `cron` needs wall time: SNTP when online; the RTC keeps counting through
      Wi-Fi outages; last known time saved to RTC memory + NVS so a soft reset
      with no uplink keeps an approximate clock. Cron jobs report "clock unset"
      in the UI rather than firing at wrong times
- [x] Engine survives a script crash: one script's error/timeout/OOM disables that
      script only; the engine task and other scripts keep running. Verified: endless loop
      stopped at 1000 ms, OOM at 256 KB, C-stack overflow as a Lua error, others unaffected.
      **And if the hub itself crashes while a script runs** (RTC marker `s_rtc_run`), the
      next boot disables that script (persisted, reason shown) — verified with a deliberate
      32 KB-stack build: one panic, next boot disabled it, the other script ran normally
- [ ] Test: unplug the router for an hour — every trigger and timer still fires,
      verified from the serial log
- [~] UI editor (built; the user checks it in the browser): paste/edit,
      Save → syntax check (`luaL_loadbuffer`) before storing, enable/disable,
      live log, last error; a load error (e.g. GPIO conflict) is shown at Save with its line.
      Automations page = scripts, pins, editor, script log, **Secrets, GPIO** (moved from
      Settings 2026-11-07; Time zone stays in Settings)
  - **Editor = CodeMirror 6** (user decision 2026-11-07; Monaco rejected: no mobile support,
    MBs). Lua highlighting, search, bracket matching, completion of the hub's API, colours
    from the page's CSS variables (dark mode). Sources + pinned versions in
    `tools/codemirror/` (`build.sh` → `components/web/ui/cm.js`, committed; npm cache and
    `node_modules` under `.esp/`), so the firmware build needs no Node. **120 KB gzipped**
    (view 177 KB + state 46 KB minified are most of it; search + autocomplete ~15 KB gz).
    Loaded only when the editor opens; the textarea is shown at once and stays if `cm.js`
    can't load. Measured over the tailnet: 0.71 s for the 120 KB, then 304 (ETag = build
    id, so once per firmware update). App image 2.0 → 2.2 MB
- [x] Scripts persisted in LittleFS (`/storage/scripts/*.lua`), started at boot
      after the device registry is loaded (before Wi-Fi: `net.online()` false at start,
      then `net up`)
- [~] Built-in examples (in the UI, not run) (door opens → buzzer + Alexa URL with `retry_for`; weekday-morning
      `cron` that logs which doors are still open)

#### Phase 5 findings (2026-11-07)
- **A script crashed the hub into a boot loop.** The engine task had 32 KB of stack "sized
  for `LUAI_MAXCCALLS=100`" — but Lua bounds the *number* of nested C levels, not their
  size. A `string.gsub` callback level costs ~1.15 KB (gsub's frame alone is 880 B), so a
  script nesting pcall/gsub/sort overflowed the stack (`Stack protection fault`, task
  `auto`); the script was saved as enabled, so every boot crashed ~2 s in, before Wi-Fi
  (USB reflash needed). Measured worst case at 160 KB: **116.4 KB** (gsub nested to the
  limit, uncaught, traceback at the deepest point); mixed pcall/gsub/sort 73 KB; also tried:
  `__tostring`, `string.format`, `__index`, `table.sort`, coroutines, deep patterns — all
  end in a clean "C stack overflow" error. Stack now **160 KB in PSRAM**
- **Pre-auth crash in the web server (fixed)**: cJSON recurses 64 B per nesting level up
  to `CJSON_NESTING_LIMIT` (default 1000). A 255-byte login body of `[` (LAN mode, before
  the password) needed ~16 KB on the 8 KB httpd stack. `CONFIG_CJSON_NESTING_LIMIT=32`
  (Kconfig of the managed component) bounds every parse site: web bodies, `json.decode`
  of HTTP responses, microlink (netmap still parses). Verified: 4000 `[` to
  `/api/auto/tz` → 400, hub up
- A firmware that crashes on its trial boot is rolled back before it can be tested —
  test builds that are meant to crash must go over USB (or expect the rollback)
- Internal heap with the engine (no scripts): **74 KB free, lowest 37–40 KB** (Phase 4:
  78.5 / 41.5); the engine and HTTP task stacks are in PSRAM. HTTPS from a script: no
  visible change in the low-water mark (mbedTLS in PSRAM)
- App image 2.0 MB (50 % of a slot); UI 19.1 KB gzipped

### Phase 5b — The user's own alarm script (user request 2026-11-07)
Built on Phase 5, before Home Assistant: it runs entirely on the hub, so it needs
nothing from Phase 6.
- [ ] Door sensors (the two contact sensors) → the Phase 1b alarm on both buzzers
      (`gpio.pattern`) and the Alexa routine URL (`secret(...)`, `retry_for`)
- [ ] Decide when writing it (user, 2026-11-07): buzzer always + Alexa as an addition,
      or buzzer only when Alexa can't be reached. What the hub can know: network down /
      timeout / error = certainly not delivered; HTTP 200 = Amazon's service accepted
      it, **not** that an Echo played (no public API reports that). Option to research:
      the routine's last step switches a smart-home device the hub can see, confirming
      the routine ran (still not that sound came out — a muted Echo completes it too)
- [ ] Arm / disarm (how: decided with the script — e.g. a schedule, or a state
      the UI sets)
- [ ] Verified on hardware: door opens → buzzers within a second, Alexa plays; same
      with the router unplugged (buzzers immediately, Alexa once it's back)

### Phase 6 — Home Assistant mode (ESPHome native API)
**Two modes, not either/or (user decision 2026-11-07).** With the integration on, the
hub always speaks to Home Assistant; what it shows depends on whether any Lua script
is enabled:
- **Devices mode** — setting on, **no script enabled**: every Zigbee device appears
  in HA as its own ESPHome sub-device of the hub (HA is the brain)
- **Smart-device mode** — setting on, **scripts enabled**: the hub appears in HA as
  **one smarter device** (the hub is the brain); the Zigbee devices aren't exposed
  individually

- [ ] **Setting** "Home Assistant integration", **off by default**, in the UI
      (Settings) and as `flash.sh --ha` at provisioning. Off → the API server
      never starts, port 6053 closed, nothing advertised
- [ ] **Mode rule** when the setting is on: devices mode iff the number of *enabled*
      scripts is 0, else smart-device mode. Disabled scripts don't count, so
      "disable all" hands HA the individual devices without deleting anything.
      Evaluated at boot, on every script enable/disable/delete, and when the setting
      changes
- [ ] Switching modes: the API server keeps running; the entity set changes, so the
      hub closes its API clients (each client task closes its own socket — lesson 8)
      and HA reconnects and lists the new entities by itself. Enabling the first
      script asks first ("Home Assistant will see the hub as one device instead of
      N devices"). Entity keys are derived from IEEE address + attribute (devices
      mode) and script name + entity name (smart-device mode), so HA entity IDs and
      history are identical every time a mode comes back
- [ ] **Smart-device mode entities (user decision 2026-11-07)**: the gateway
      entities below; one switch per script (enable/disable);
      entities a script publishes through a small Lua API (e.g. `ha.sensor(name,
      value)`, `ha.binary(name, on)`, `ha.event(name)`), so a script can expose
      "alarm armed", "doors open: 2", "alarm triggered" instead of raw devices
- [ ] ESPHome native API on TCP 6053, **Noise encryption mandatory** (no plaintext).
      Key generated by the board the first time the setting is turned on (or by
      `flash.sh --ha`, stored in `.esp/secrets/` and written to `prov`); shown
      in the UI for pasting into HA, regenerable
- [ ] mDNS `_esphomelib._tcp` so HA discovers the hub automatically on the LAN.
      Over the tailnet (HA elsewhere) mDNS doesn't cross: add it in HA by
      MagicDNS name, `zigbee-hub.<tailnet>.ts.net:6053`. In tailnet-only access mode
      the API accepts only tailnet connections (same check as the web server) and
      nothing is advertised
- [ ] **Sub-devices** (devices mode; HA/ESPHome 2025.7+): hub = main device, each
      Zigbee device = sub-device with its own name/area/manufacturer/model from the
      interview. `device_id` field numbers per entity type are in the reference
      repo's CLAUDE.md ("Protobuf device_id Field Numbers") — verify against the
      current `api.proto` before relying on them
- [ ] Capability → entity mapping (devices mode; same capabilities as the UI uses):
      sensors → sensor with device_class, IAS zone → binary_sensor,
      battery/LQI/last seen → diagnostic entities. Read-only: the hub doesn't command
      Zigbee devices (no `zb.set`, user decision 2026-11-07)
- [ ] Gateway entities (both modes): permit-join button (pairing works from HA too),
      Zigbee channel, device count, Wi-Fi RSSI, uptime
- [ ] Wire-format safety: every length off the socket bounded before use
      (reference had a pre-auth overflow on the hello frame — lesson 9)
- [ ] Entity table in PSRAM (reference: 54 KB, moving it freed half the internal heap)
- [ ] The web UI keeps working in both modes (devices, enrollment, network, status);
      the Automations page shows which mode is active and why
- [ ] When the setting is off, the ESPHome API code costs no runtime RAM (server
      task and entity table only allocated when it's on)
- [ ] Verified: HA discovers the hub, adopts with the key, shows each paired device as
      a sub-device (no script enabled); enabling a script turns it into one device
      with the smart-device entities; disabling all scripts brings the individual
      devices back with history intact

### Phase 7 — Hardening
- [ ] Soak test 24 h+ (reference repo's worst bugs needed 9 h of uptime)
- [ ] Internal heap low-water mark measured in both modes: Zigbee + Tailscale +
      UI client + 3 scripts, and Zigbee + Tailscale + UI client + HA connected
- [ ] Zigbee network backup/restore (download from UI)
- [ ] Factory reset (UI + long-press BOOT button)

## Internal RAM

**Policy (user, 2026-11-04): reclaim only when a phase actually needs it** — the
hardware is there to be used. No pre-emptive audits or cuts. Every phase still
records free and lowest internal heap in its verification notes, so a shortage is
seen coming.

Where it stands: after Phase 2, **133 KB free, lowest 98 KB** (Wi-Fi + web UI +
WebSocket client + on-board log); with Zigbee (Phase 3) **95 KB free, lowest 63 KB**;
with Tailscale (Phase 4, 2026-11-07: Zigbee + Wi-Fi + web + Tailscale with 3 DERP
connections, a UI WebSocket client, IPv6 off) **78.5 KB free, lowest 41.5 KB** (the
lowest is early in boot). Pulled so far: lever 3 (mbedTLS in PSRAM) and lever 4 for
microlink's four tasks and the two Phase 4 watcher tasks (none of them writes flash).
With the Lua engine (Phase 5, 2026-11-07, no scripts): **74 KB free, lowest 37–40 KB** —
the engine's 160 KB stack is in PSRAM, so it cost ~4 KB internal, not the 8–16 KB expected.

Levers, roughly cheapest first — pull only what's needed:
1. Wi-Fi code out of IRAM (`CONFIG_ESP_WIFI_IRAM_OPT`, `_RX_IRAM_OPT`,
   `_SLP_IRAM_OPT` = n; the boot log shows all three on). On the C5, IRAM code and
   heap share the same 320 KB. Costs Wi-Fi throughput we don't need. Est. tens of KB
2. Fewer static Wi-Fi RX buffers (`CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM`, now 16
   × ~1.7 KB). 6–8 is plenty here. Est. ~14 KB
3. mbedTLS allocations in PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`). IDF's warning
   about PSRAM on rev v1.0 only matters with flash encryption, which we don't use —
   anyone with the board can read the flash anyway. Biggest win for Tailscale
4. Task stacks in PSRAM (`CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` +
   `xTaskCreateWithCaps`) — only for tasks that never write flash/NVS (audit each)
5. Measure first when a lever is needed: per-task stack high-water marks,
   `heap_caps_print_heap_info`, `idf.py size-components`

## Lessons carried over from the reference repo

All measured there on the same chip; re-verify, don't assume.

1. **Coex before the first association** (`esp_coex_wifi_i154_enable()`).
2. **Avoid DFS 5 GHz channels**; 36–48 are fine.
3. **authmode threshold is a floor**, and the enum isn't ordered by strength. Use WPA2.
4. **The C5 periodically loses its AP** (reason 201 NO_AP_FOUND at −50 dBm); a
   watchdog that recovers it is a mitigation. Never reset the watchdog from the
   event it watches. (Unlike the reference, our final reboot waits for a safe moment.)
5. **Wi-Fi scan results on the C5 are unreliable** — don't build features on scans.
   (Also: an all-zero `wifi_scan_config_t` is an *empty* scan; pass `NULL` for a full one.)
6. **Internal RAM is the bottleneck, PSRAM sits idle.** Route cJSON to PSRAM
   (`cJSON_InitHooks`), put big tables in PSRAM. Find candidates with
   `riscv32-esp-elf-nm --print-size --size-sort`. Not for ISR or cache-disabled access.
7. **The heap low-water mark happens early in boot**, not at steady state; measure there.
8. **A socket is closed by the task that reads it** — never `close()` from another
   task while one sits in `recv()` (crashed lwIP there).
9. **Bound every length read off the wire** (reference had a pre-auth overflow
   from an unchecked 16-bit length).
10. **`sdkconfig.defaults` changes don't apply** to an existing `sdkconfig`. Delete
    `sdkconfig` + `build/` and check `build/config/sdkconfig.h`.
11. **Zigbee calls from other tasks need the Zigbee lock**; don't do file I/O
    while holding it.
12. **`esp_log_level_set("wifi", ...)` hides the diagnostics** — go VERBOSE when
    Wi-Fi misbehaves.

## Decisions (user; 2026-11-04 unless dated)

- **Tailscale = [microlink](https://github.com/CamM2325/microlink)**, and we're free
  to modernise it: port to IDF v6.1 / mbedTLS 4 / PSA, newer deps, drop what we
  don't need. It's our fork once copied into `components/tailscale/`.
- **"Scanning" = discovery, not QR.** Success criterion: *put the battery in a
  random cheap reed sensor and the hub finds it.* No camera, no install codes.
- **HA modes** (opt-in setting on; updated 2026-11-07): zero *enabled* scripts →
  every Zigbee device as its own HA device; scripts enabled → the hub as one smarter
  HA device. Disabled scripts don't count.
- **Lua** for automations (see Phase 5).
- **Script editor = CodeMirror 6**, prebuilt and committed (2026-11-07; Monaco rejected:
  no mobile support, several MB).
- **GPIO pins are claimed only while a script loads** (2026-11-07), so conflicts show at
  Save. Secrets and GPIO settings live on the Automations page (2026-11-07).
- **The hub never commands Zigbee devices** (2026-11-07): no `zb.set`, nothing from HA.
- **No last-known clock in NVS** (2026-11-07): after a power-off it would be wrong; cron
  waits for SNTP instead. Soft resets keep the RTC time.
- **Router**: test on DFS channel 52 first; move to 36–48 only if measurements are bad.
- **Tailnet-only access**: once Tailscale works, an option to drop the password
  and listen only on the Tailscale interface (Phase 4, "Access mode").

## Conventions

- C, `snake_case`, statics prefixed `s_`, every `esp_err_t` checked
- One component per concern; components talk through the event bus, not globals
- Lengths from the network are bounded before use
- Each phase's verification evidence (log excerpt, numbers) goes into this file
