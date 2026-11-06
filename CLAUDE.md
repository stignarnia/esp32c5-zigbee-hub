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
6. **Optional Home Assistant mode when there are no automations** — an opt-in
   setting (off by default). When it's on and no script is enabled, the board
   speaks the **ESPHome native API** and every Zigbee device appears in Home
   Assistant as a native sub-device of the hub. Either the hub is the brain
   (scripts) or Home Assistant is; never both at once.

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
6. **HA mode doesn't change the priority.** The ESPHome API server is a consumer
   of the device registry like the UI; it must never block Zigbee. With HA as the
   brain, automations depend on Wi-Fi by definition — that's the user's choice by
   having no local scripts.

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
            │  zb_*        │  + persistence │  ── or ──       │  WireGuard netif   │
            │              │  (LittleFS)    │  ESPHome API    │  in lwIP           │
            │              │                │  (HA, :6053)    │                    │
            ├──────────────┴────────────────┴─────────────────┴────────────────────┤
            │  Wi-Fi manager (STA, 5 GHz only)  ·  coex  ·  provisioning (NVS)     │
            └──────────────────────────────────────────────────────────────────────┘
```

The HTTP server binds to all interfaces. microlink adds its WireGuard interface to
lwIP's netif list, so the same server answers on the LAN IP and on the `100.x`
tailnet IP without extra code (to verify in Phase 4).

### Source layout (to create)

```
CMakeLists.txt  sdkconfig.defaults  partitions.csv  flash.sh  env.sh
main/            app_main, boot order
components/
  prov/          read provisioning NVS partition
  wifi5g/        STA manager, 5 GHz only, watchdog
  zb/            coordinator, enrollment, interview, ZCL helpers
  devices/       registry, capabilities, persistence
  web/           http server, REST, WebSocket, embedded assets
  web/ui/        index.html, app.js, style.css (no build step, gzipped by CMake)
  automation/    Lua engine, bindings, script store
  lua/           Lua 5.5 source (MIT), vendored
  tailscale/     microlink (MIT) ported to IDF v6, wrapped
  esphome_api/   ESPHome native API server (Noise), HA mode
tools/           host helpers (provisioning image generation)
```

### Partition table (16 MB)

Eight partitions, contiguous. Small ones packed first, ending on the 64 KB boundary
the app slots need; app slots and LittleFS take everything else (2026-11-06).

| Name | Type | Offset | Size | Purpose |
|---|---|---|---|---|
| nvs | nvs | 0x9000 | 0x4000 | app settings, Wi-Fi driver state, radio calibration (~3 KB used, measured) |
| otadata | ota | 0xD000 | 0x2000 | |
| prov | nvs | 0xF000 | 0x3000 | **written by flash.sh**: SSID, PSK, admin password, TS auth key, hostname, optional HA setting + API key (< 1 KB) |
| zb_storage | nvs | 0x12000 | 0x1E000 | Zigbee stack NVRAM (network key, PAN, children). esp-zigbee-lib 2.x needs NVS, not FAT. Generous on purpose: resizing it later means re-pairing every device |
| coredump | coredump | 0x30000 | 0x10000 | ELF core dump on panic; summary logged on next boot, `/api/coredump` |
| ota_0 | app | 0x40000 | 0x400000 | |
| ota_1 | app | 0x440000 | 0x400000 | |
| storage | littlefs | 0x840000 | 0x7C0000 | device registry, scripts, logs, UI overrides, optional converter DB |

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
  - `--backup-ssid/--backup-psk` (or `--no-backup`), `--hostname`, `--ts-authkey`;
    remembered in `.esp/secrets/prov.env` (0600) for the next run
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
  - [x] watchdog reboot logged on the next boot; [ ] shown in the UI (Phase 2)
  - [ ] gate implemented (Phases 3/5: timers, pairing, OTA, state flushed)
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
| ping while associated | not reported | ~60 ms avg (modem sleep, needed for coex) vs 7–10 ms without Zigbee |

### Phase 1b — Buzzer & GPIO hardware validation (user request)
The user's buzzers: **RUNCCI-YUN "DC 3V active piezo, continuous sound"**, 20 pcs
(Amazon B0FMXS7H4N; specs hidden behind the cart, no datasheet found). "Piezo"
in such listings is often wrong — they may be 12 mm electromagnetic types
(~20–30 mA at 3 V) rather than true piezo (a few mA). **No other parts available**
(no transistors, resistors, wires, multimeter), so the board measures itself.

Independent of the other phases; its results feed Phase 5's `gpio.*` API.

- [ ] **Inputs from the user**: photo of a buzzer (side + bottom: piezo vs magnetic,
      leg spacing) and of the board (pin labels)
- [ ] **Pin choice**, from the ESP32-C5 datasheet + this board's pinout (not memory):
  - not flash/PSRAM, USB D−/D+, UART0 (GPIO 11/12), 32 kHz crystal, or strapping pins
  - **stays low through reset and boot** — otherwise the buzzer chirps on every
    reboot (and a stuck-high pin during a crash would be a fail-safe hole)
  - ADC-capable (for the self-measurement below)
  - a GND pin at a spacing the buzzer legs can reach without wires
- [ ] **Probe firmware** as a separate tiny project (`tools/gpio_probe/`), flashed
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
- [ ] **Decision rule** (recorded here with the numbers):
  - loaded voltage ≥ 3.0 V → direct drive OK, any drive strength that's loud enough
  - 2.5–3.0 V → direct drive OK at max drive strength, beeps only (no long tones),
    duty-cycle limited in `gpio.beep`
  - < 2.5 V → too much current for a pin: needs a transistor (BC547/2N2222 + 1 kΩ)
- [ ] Result becomes Phase 5's defaults: allowed pin list, drive strength, and any
      on-time/duty limits `gpio.beep` enforces for this buzzer type

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
- [~] **Confirmation rule, Phase 2 version**: confirmed once Wi-Fi has an IP and
      the web server runs, held 5 s; not healthy within 120 s → `esp_restart()` →
      bootloader rolls back. Phase 3 replaces this with "Zigbee is up". Known
      cost: an update installed while the router is down 2+ min rolls back too
- [x] Planned reboots disconnect Wi-Fi cleanly first (`wifi5g_prepare_reboot`), so
      the next boot doesn't hit the stale-association login delay
- [ ] **Reboot gate** ("safe moment": timers/pairing) — nothing to gate yet;
      lands with Phase 5 (timers) and Phase 3 (pairing)
- [x] `flash.sh --ota [--host H]` (default `<hostname>.local`; MagicDNS name
      after Phase 4): build → login (password via stdin) → upload → wait →
      report. **Measured: upload 5.6 s, confirmed 16 s after upload start, ~21 s
      for the whole command when nothing needs rebuilding.** Faster than USB
- [ ] Rollback demonstrated with a deliberately broken image (not yet)
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

### Phase 3 — Zigbee coordinator + enrollment (in progress 2026-11-06)
- [x] `espressif/esp-zigbee-lib` **2.1.0** (released 2026-10-30, built against IDF
      5.5.6) builds and links on IDF 6.1. Stack NVRAM in the NVS `zb_storage` partition
- [x] **Coex before Wi-Fi connects**: `esp_coex_wifi_i154_enable()` in `zb_start()`,
      before `wifi5g_start()`
- [~] **Coex priority — measured 2026-11-06** (802.15.4 PTI via `esp_ieee802154_set_coex_config`):
  - txrx/txrx_at HIGH permanently: Wi-Fi never associates (NO_AP_FOUND after 15 s,
    every attempt). A coordinator is always receiving, so HIGH never yields
  - driver defaults (idle 4, txrx LOW, txrx_at MIDDLE) from boot: network formation
    never completes, with Wi-Fi `PS_NONE` or `PS_MIN_MODEM`
  - **chosen**: HIGH until the network is formed/restored, defaults after.
    Formation 576 ms, restore 8 ms (done before Wi-Fi starts)
  - Wi-Fi in **modem sleep** (`WIFI_PS_MIN_MODEM`, as Espressif's gateway does with
    coex). Ping avg 7 → ~60 ms
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
      the page polls every 1 s. Permit-join round trip verified (open 10 s → 9 s left → closed)
- [x] OTA health = Zigbee up + web server (Wi-Fi no longer required)
- Internal heap with Zigbee + Wi-Fi + web (no client): **95 KB free, lowest 63 KB**
  (Phase 2: 133 / 98) — Zigbee costs ~40 KB
- [ ] Enrollment ("Scan for devices" in the UI):
  - permit-join for N seconds (default 120) with live countdown over WebSocket
  - live list of `device_announce` events as they arrive
  - interview: node descriptor, active endpoints, simple descriptors, Basic
    cluster (manufacturer, model, power source); progress streamed per step
  - bind + configure reporting for supported clusters
    (reference bug: never initialised → no reports ever arrived)
  - user names the device and assigns a room; persisted to LittleFS
    **including the cluster list** (reference bug: restored devices had none)
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
- [ ] Install-code join (nice-to-have): paste a Zigbee 3.0 install code
- [ ] Capability mapping from clusters (generic first):
      On/Off, Level, Color Control (CT + XY), Temperature, Humidity, Pressure,
      Illuminance, Occupancy, IAS Zone (contact/motion/water/smoke), Power Config
      (battery), Electrical Measurement, Metering, Window Covering, Thermostat
- [~] Device page: live state, LQI/RSSI, last seen, availability, remove/re-interview.
      **No controls in the UI (user decision 2026-11-06)**: switching is done by Lua
      (`zb.set`) or Home Assistant
- [ ] Availability tracking (power-source-aware timeouts)
- [ ] Network map from neighbour tables (LQI)
- [ ] Zigbee lock (`esp_zb_lock_acquire`) around every stack call from other tasks
- [ ] Later, optional: converter DB for quirky devices (Tuya DP etc.), runtime
      JSON in LittleFS — the reference repo's `data/converters_merged` (z2m-derived,
      MIT + Apache notices) is the obvious source

### Phase 4 — Tailscale
- [ ] **Spike first**: build microlink against IDF v6.1. It uses legacy mbedTLS
      APIs (`chacha20.h`, `chachapoly.h`, `ctr_drbg.h`, `entropy.h`); IDF v6 ships
      mbedTLS 4.x, where much of that moved behind PSA. Port to PSA (user OK'd
      modernising it freely). This is the riskiest item in the plan — do it
      before the UI gets big
- [ ] Copy microlink + wireguard_lwip into `components/tailscale/` (MIT, keep
      `LICENSE` + `x25519-license.txt`); drop cellular/PPP/AT-socket code
- [ ] We own Wi-Fi: call `microlink_start()` after our STA has an IP; never let
      `ml_net_switch` initialise Wi-Fi (it would not set 5G-only)
- [ ] Buffers (H2 512 KB, JSON 512 KB) in PSRAM; task stacks ~42 KB internal —
      check internal heap budget against Zigbee + Wi-Fi + HTTP
- [ ] Disable microlink's own config httpd; its settings move into our UI
      (auth key, hostname, control server for Headscale, peer allowlist)
- [ ] UI page: state, tailnet IP, MagicDNS name, peers with direct/DERP path
- [ ] Verified: `tailscale ping zigbee-hub` from the dev machine; UI loads at
      `http://zigbee-hub/` from a phone on cellular

#### Access mode: "Tailnet only, no password" (user request)
Offered in the UI only once Tailscale is connected. When on:
- [ ] Web UI, `/api`, `/ws`, OTA, and (if active) the ESPHome API accept
      connections **only via the Tailscale interface**, and the admin password is
      not asked. Tailscale is the authentication: only tailnet members can reach it
- [ ] Enforcement, two layers (lwIP is a *weak host*: by default it accepts a
      packet for any of its IPs on any interface, so "the destination is 100.x"
      alone proves nothing):
  1. accept-time check (`httpd_config_t.open_fn`, and the same in the ESPHome
     server): `getsockname()` must be our tailnet IP, `getpeername()` must be in
     100.64.0.0/10, else close before reading a byte
  2. lwIP input hook (`LWIP_HOOK_IP4_INPUT`): drop any packet arriving on the
     Wi-Fi netif whose destination is the tailnet IP (strong-host for that address)
- [ ] mDNS stops advertising the UI and `_esphomelib._tcp` in this mode (nothing
      on the LAN to find)
- [ ] Switching it on: UI requires the password one last time, and only offers it
      while the current request itself came over Tailscale (proves it works
      before cutting the LAN off)
- [ ] **Lockout recovery** (Tailscale key expired, tailnet down, account change):
  - hold the BOOT button 10 s → LAN access with password re-opens for 15 min
  - or USB: `flash.sh --prov-only --lan-access` resets the mode
  - automations and Zigbee are unaffected either way
- [ ] Who on the tailnet can reach it is decided by Tailscale ACLs — README says
      to restrict `zigbee-hub` to the user's own devices if the tailnet is shared
- [ ] Verified: from a LAN machine not on Tailscale, port 80 refuses/closes; from
      the tailnet the UI opens with no login; forged packets to 100.x from the LAN
      are dropped (send with `nping`/scapy from another LAN host)

### Phase 5 — Automation (Lua 5.5)

**Decided (user, 2026-11-04): Lua.** Why: most pasted scripts will be written by
the user or an AI, and Lua is widely known; the C API gives per-script memory caps
(custom allocator) and runaway-loop aborts (instruction hook); ~200–250 KB flash;
MIT. Rejected: Berry (Tasmota; lean but niche), QuickJS (full JS, viable but
heavier), mJS/Elk/Espruino (JS subsets — pasted normal JS often fails). ESPHome
has no runtime scripting (YAML compiled to C++), so nothing to borrow there.

- [ ] Vendor Lua 5.5.1 (latest at time of writing; MIT) into `components/lua/`; allocator into PSRAM with a
      per-script memory cap (default 256 KB)
- [ ] One `lua_State` per script, all run on one automation task (events are
      queued to it — Lua never runs on the Zigbee or HTTP task)
- [ ] Sandbox: no `io`, `os.execute`, `require`, `load` of bytecode; instruction
      count hook aborts runaway scripts; errors reported to the UI with line numbers
- [ ] API:
  - `zb.on(device, attr, fn)` — state change triggers
  - `zb.set(device, {state="on", brightness=128})`, `zb.get(device, attr)`
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
      verified; plain HTTP allowed for LAN targets
    - **`retry_for = seconds`**: while offline or on errors (5xx, timeouts),
      retried with backoff until that deadline — for alarms that must arrive
      once Wi-Fi is back. Default 0 = one attempt, dropped when offline
    - limits: response bodies truncated at 16 KB; queue of 16 requests;
      per-script rate limit (default 10/min); one TLS connection at a time
      (~40 KB internal RAM while it runs — measure in Phase 7)
  - **Secrets**: `secret("alexa_alarm_url")` reads a value stored under
    System → Secrets (write-only in the UI, kept in NVS, never shown in script
    text or logs). URLs with tokens belong there, not pasted into scripts;
    `log()` masks secret values if a script prints one
  - `log(...)` → UI log stream
  - **GPIO** (user request: 3 V buzzers on the board's pins):
    - `gpio.write(pin, 0|1)`, `gpio.read(pin)`, `gpio.on(pin, "rising"|"falling"|"both", fn)`
      (debounced, delivered through the automation queue — never from the ISR)
    - `gpio.beep(pin, ms)` / `gpio.beep(pin, {on=100, off=100, times=3})` for
      **active** buzzers (on/off), non-blocking, driven by a timer not the script
    - `gpio.tone(pin, hz, ms)` for **passive** buzzers (LEDC PWM)
- [ ] GPIO safety:
  - only pins enabled in the UI (System → GPIO: name, direction) are usable;
    scripts may refer to them by name (`gpio.write("buzzer", 1)`)
  - pins that are never allowed: SPI flash / PSRAM bus, USB D−/D+, UART0 console
    (GPIO 11/12), strapping pins, 32 kHz crystal pins. Exact list from the C5
    datasheet + this board's pinout when implementing — don't guess
  - one script owns a pin at a time (claim on first use; conflict = load error)
  - **fail-safe**: when a script stops (disabled, error, timeout, OOM, deleted),
    every output it owned goes low and any beep/tone stops — a crashed script
    must never leave a buzzer screaming
  - the UI GPIO page shows each enabled pin's state and owning script
- [ ] **Time without Wi-Fi**: event triggers and `timer.*` need no clock and always
      work. `cron` needs wall time: SNTP when online; the RTC keeps counting through
      Wi-Fi outages; last known time saved to RTC memory + NVS so a soft reset
      with no uplink keeps an approximate clock. Cron jobs report "clock unset"
      in the UI rather than firing at wrong times
- [ ] Engine survives a script crash: one script's error/timeout/OOM disables that
      script only; the engine task and other scripts keep running
- [ ] Test: unplug the router for an hour — every trigger and timer still fires,
      verified from the serial log
- [ ] UI editor: paste/edit (textarea with line numbers; no heavy editor),
      Save → syntax check (`luaL_loadbuffer`) before storing, enable/disable,
      live log, last error
- [ ] Scripts persisted in LittleFS (`/storage/scripts/*.lua`), started at boot
      after the device registry is loaded
- [ ] Built-in examples (motion → light with timeout; sunset-ish schedule)

### Phase 6 — Home Assistant mode (ESPHome native API)
- [ ] **Setting** "Home Assistant integration", **off by default**, in the UI
      (System page) and as `flash.sh --ha` at provisioning. Off → the API server
      never starts, port 6053 closed, nothing advertised
- [ ] **Rule** when the setting is on: API server runs iff the number of
      *enabled* scripts is 0. Disabled scripts don't count, so "disable all" hands
      control to HA without deleting anything. Evaluated at boot, on every script
      enable/disable/delete, and when the setting changes
- [ ] Turning the setting on with scripts enabled: allowed, UI shows
      "HA integration waiting: N scripts enabled"
- [ ] Switching modes from the UI:
  - enabling the first script → confirm dialog ("Home Assistant will lose access
    to N devices"), then close API clients (each client task closes its own
    socket — lesson 8), stop the server, withdraw the mDNS service
  - disabling the last script → start the server, advertise again; HA reconnects
    on its own
  - entity keys are derived from IEEE address + attribute, so HA entity IDs and
    history are identical every time the hub comes back into HA mode
- [ ] ESPHome native API on TCP 6053, **Noise encryption mandatory** (no plaintext).
      Key generated by the board the first time the setting is turned on (or by
      `flash.sh --ha`, stored in `.esp/secrets/` and written to `prov`); shown
      in the UI for pasting into HA, regenerable
- [ ] mDNS `_esphomelib._tcp` so HA discovers the hub automatically on the LAN.
      Over the tailnet (HA elsewhere) mDNS doesn't cross: add it in HA by
      MagicDNS name, `zigbee-hub.<tailnet>.ts.net:6053`
- [ ] **Sub-devices** (HA/ESPHome 2025.7+): hub = main device, each Zigbee device
      = sub-device with its own name/area/manufacturer/model from the interview.
      `device_id` field numbers per entity type are in the reference repo's
      CLAUDE.md ("Protobuf device_id Field Numbers") — verify against the current
      `api.proto` before relying on them
- [ ] Capability → entity mapping (same capabilities as the UI uses):
      on/off → switch, on/off+level(+color) → light, sensors → sensor with
      device_class, IAS zone → binary_sensor, covering → cover, thermostat →
      climate, battery/LQI/last seen → diagnostic entities
- [ ] Commands from HA go through the same device command path as Lua
      (one path, two callers)
- [ ] Gateway entities: permit-join button (pairing works from HA too), Zigbee
      channel, device count, Wi-Fi RSSI, uptime
- [ ] Wire-format safety: every length off the socket bounded before use
      (reference had a pre-auth overflow on the hello frame — lesson 9)
- [ ] Entity table in PSRAM (reference: 54 KB, moving it freed half the internal heap)
- [ ] The web UI keeps working in HA mode (devices, enrollment, network, system);
      the Automations page shows which mode is active and why
- [ ] When the setting is off, the ESPHome API code costs no runtime RAM (server
      task and entity table only allocated when the mode activates)
- [ ] Verified: HA discovers the hub, adopts with the key, shows each paired
      device as a sub-device; toggling a light from HA works; enabling a script
      makes HA show them unavailable; disabling it brings them back with history intact

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
WebSocket client + on-board log); with Zigbee (Phase 3) **95 KB free, lowest 63 KB**. Expected next (estimates, not measured here):
Zigbee ~60–80 KB, Tailscale ~42 KB of task stacks + ~30–40 KB for its persistent
TLS connection, Lua engine task ~8–16 KB. That adds up to more than what's free,
so expect to pull levers during Phase 3 or 4.

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

## Decisions (user, 2026-11-04)

- **Tailscale = [microlink](https://github.com/CamM2325/microlink)**, and we're free
  to modernise it: port to IDF v6.1 / mbedTLS 4 / PSA, newer deps, drop what we
  don't need. It's our fork once copied into `components/tailscale/`.
- **"Scanning" = discovery, not QR.** Success criterion: *put the battery in a
  random cheap reed sensor and the hub finds it.* No camera, no install-code UI
  required (install codes stay a nice-to-have).
- **HA mode trigger** (opt-in setting on) = zero *enabled* scripts; disabled
  scripts don't count.
- **Lua** for automations (see Phase 5).
- **Router**: test on DFS channel 52 first; move to 36–48 only if measurements are bad.
- **Tailnet-only access**: once Tailscale works, an option to drop the password
  and listen only on the Tailscale interface (Phase 4, "Access mode").

## Conventions

- C, `snake_case`, statics prefixed `s_`, every `esp_err_t` checked
- One component per concern; components talk through the event bus, not globals
- Lengths from the network are bounded before use
- Each phase's verification evidence (log excerpt, numbers) goes into this file
