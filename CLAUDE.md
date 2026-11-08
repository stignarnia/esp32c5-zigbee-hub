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
   When it's on, the board speaks the **ESPHome native API**: every Zigbee device
   appears in Home Assistant as a native sub-device of the hub, and the hub adds its own
   entities plus whatever the enabled scripts publish (see Phase 6).

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
   consumer of the device registry like the UI; it must never block Zigbee. Automations
   built in HA depend on Wi-Fi by definition — the user's choice; local scripts run on
   the hub whatever HA does.

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
| Board | Waveshare ESP32-C5-WIFI6-KIT-NXRX, [schematic](https://files.waveshare.com/wiki/ESP32-C5-WIFI6-KIT-NXRX/ESP32-C5-WIFI6-KIT-NXRX-Schematic.pdf); bare plated holes, no header pins |
| eFuses (read 2026-11-08) | factory: `UART_PRINT_CONTROL` 0, `JTAG_SEL_ENABLE` 0, `DIS_PAD_JTAG` 0, `DIS_USB_JTAG` 0. `espefuse` leaves the chip in download mode: reset it afterwards (RTS pulse) or the hub stays down |

The user is in group `uucp` (serial access).

### GPIO map of this board (2026-11-08; schematic + ESP32-C5 datasheet v1.5 §2, §3)

"At reset" = what the chip reads from the pin at reset, with the eFuses above. The
datasheet's strapping list is GPIO2, 3, 7, 25, 26, 27, 28 (IDF's docs list only 2, 7,
25, 27, 28). Its restriction list says "GPIO27: the VDD_SPI pin" — a typo: its pin table
has VDD_SPI = pin 29 = GPIO19.

| GPIO | On the board | Chip function | At reset | Scripts |
|---|---|---|---|---|
| 0 | header P1-5; 32 kHz crystal Y1, R12, R17 **NC (not fitted)** | XTAL_32K_P | — | yes (buzzer 3 −) |
| 1 | header P1-6; crystal not fitted | XTAL_32K_N, ADC1_CH0 | — | yes |
| 2 | header P1-3 | MTMS (JTAG TMS), ADC1_CH1 | strapping (floating), but no boot setting in §3 uses it | yes (buzzer 3 +) |
| 3 | header P1-4 | MTDI (JTAG TDI), ADC | SDIO-slave sampling/driving edge (with 25): never used | yes |
| 4 | header P2-8 | MTCK (JTAG TCK), ADC; pull-up after reset | — | yes |
| 5 | header P2-9 | MTDO (JTAG TDO), ADC1_CH4 | — | yes (buzzer 1 +) |
| 6 | header P1-7 **and BAT_ADC**: R39 0 Ω from the 200 k/100 k battery divider + C14 100 nF | ADC | — | yes (buzzer 2 +); with a battery fitted, driving it loses the reading |
| 7 | header P1-8, no pull resistor | — | JTAG source: ignored (`JTAG_SEL_ENABLE` 0) | yes |
| 8, 9, 10 | header P1-9/10/11 | — | — | yes (8 = buzzer 2 −) |
| 11 | header P2-2 "TX", R37 0 Ω → CH343 RXD | U0TXD | carries the boot log | **no** |
| 12 | header P2-3 "RX", R33 0 Ω ← CH343 TXD (driven) | U0RXD, pull-up | — | **no** |
| 13, 14 | header P2-14/13, R21/R23 22 Ω → "USB" USB-C (D−/D+) | USB Serial/JTAG; 14 has the USB pull-up at reset | — | **no** |
| 15 | header P2-6 "NC/GPIO15" | **SPICS1, PSRAM chip select** | — | **no** (CPU_LOCKUP, 2026-11-08) |
| 16–18, 20–22 | inside the module | SPI0/1 flash + PSRAM bus | — | **no** |
| 19 | inside the module | VDD_SPI (flash supply) | — | **no** |
| 23, 24 | header P2-5/4 | — | — | yes |
| 25 | header P1-13 | — | SDIO-slave edge (with 3): never used | yes |
| 26 | header P1-12 | — | boot mode only with 27 = 0 and 28 = 0 (BOOT held) | yes |
| 27 | header P2-7, R8 10 k pull-up, R3 0 Ω → **WS2812B RGB LED** (LED2) DI | — | ROM UART printing (ignored, eFuse 0); with BOOT held 1 = download mode 0 (pull-up holds it) | **ground only** (buzzer 1 −) |
| 28 | header P2-11, R9 10 k pull-up, **BOOT** button Key2, auto-download transistor T1 (DTR/RTS) | — | **SPI boot (1) or download (0)** | **no** |

Allowed for scripts: **0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 23, 24, 25, 26; 27 as ground
only** (user decision 2026-11-08). GPIO 0/1 only because this board leaves the crystal
out. Auto-download (schematic table): DTR/RTS equal → EN 1, BOOT 1 (no reset); RTS
alone → EN 0 (reset); DTR alone → BOOT 0.

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
  motion/        Wi-Fi motion sensing (CSI): moving / not moving, two-button calibration
  esphome_api/   (Phase 6, not yet) ESPHome native API server (Noise), HA integration
examples/        example Lua scripts offered in the editor (*.lua; put into app.js at
                 build time by components/web/gen_app_js.py; template.lua = new-script text)
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
    legs ~7.6 mm apart by eye (= 3 header holes) — **wrong: the legs are natively 2 holes
    apart (5.08 mm), user 2026-11-08**. The usual "12 mm active buzzer"
    package, typically electromagnetic (~20–30 mA at 3 V) despite "piezo" in the
    listing; the probe measures it instead of guessing
  - board: **Waveshare ESP32-C5-WIFI6-KIT-NXRX** (ESP32-C5-WROOM-1, N16R8). Headers:
    top `G TX RX 24 23 N/15 27 4 5 NC 28 G 14 13 G VBAT`, bottom
    `3V3 RST 2 3 0 1 6 7 8 9 10 26 25 5V G VBAT`. On board: RGB LED on IO27, BOOT on
    IO28, two USB-C ports ("UART" = CH343, "USB" = native USB on GPIO13/14)
- [x] **Pin choice** (redone 2026-11-08 from the datasheet + this board's schematic: see
      "GPIO map of this board" in the Hardware section for every pin and why). The buzzer
      legs are natively 2 holes apart. Final placement: buzzer 1 = + IO5 / − IO27 (ground),
      buzzer 2 = + IO6 / − IO8 (ground), buzzer 3 = + IO2 / − IO0 (ground). What a buzzer pin must satisfy:
    - not flash/PSRAM (15–22), USB D−/D+ (13/14), UART0 (11/12) or BOOT (28)
    - **stays low through reset and boot**, otherwise the buzzer chirps on every reboot (and
      a stuck-high pin during a crash would be a fail-safe hole). Checked by ear: no chirp
      across resets with all three buzzers plugged in (software restart and USB hardware reset)
    - ADC-capable for the + pin if the probe should measure the load (GPIO1–6)
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
    pad supply. (Correction 2026-11-08: no custom bootloader needed — the enable bits,
    `LP_ANA_POWER_GLITCH_CNTL_REG.PWR_GLITCH_RESET_ENA`, one per supply domain, are R/W
    from the app. The detectors are an anti-fault-injection feature; not changed)
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
- [x] **Follow-up 2026-11-08: the hub reset-looped, the pins changed** (final pins:
      **buzzer 1 = + IO5 / − IO27, buzzer 2 = + IO6 / − IO8, buzzer 3 = + IO2 / − IO0**)
  - `t_buzz` (the alarm, played at script start) on the hub: `RTC_PWR_GLITCH` ~1.7 s into
    every boot → reboot loop, Zigbee down, until the buzzers were pulled out (buzzer 1 then
    on IO5 → GND, 3 holes apart: the legs, natively 2 apart, had to be bent)
  - the board has **bare plated holes, no header pins**: a ~0.5 mm buzzer leg in a ~1 mm
    hole touches only where it leans, so contact makes and breaks with any movement
  - Phase 1b repeated with the probe: 50/50 rounds, 0 resets, radio off; then with Wi-Fi
    associated (ch 36, −47 dBm, pinging the gateway every 50 ms, 2163/2163): 50/50, 0
    resets. **Wi-Fi is not what made the hub reset** (802.15.4 not tested)
  - **breadboard + unsoldered header pins: worse.** The pins stand centred in the board's
    holes and make contact only while the board is pressed down (IO6 read 3357 mV driven
    = no load; sounds when pressed, user). Dropped. Without solder, the legs go straight
    into the board
  - user rule: each buzzer in two holes **2 apart** (the legs' native spacing, no bending)
    and **on the antenna side of the IO9/NC row**; the case holds them.
    The only pairs under the first allow list: IO6–IO8, and IO5–IO27 with IO27 as a ground.
    With 0–10 allowed, 2–0, 3–1 and 1–7 qualify too, but their 12 mm bodies crowd buzzer 2;
    the user fitted **buzzer 3 = + IO2 / − IO0** (closest of them, 4 holes from buzzer 2;
    verified silent across software and USB hardware resets)
  - **IO15 must never be used** (removed from every pin list): its pad is **SPICS1, the
    in-package PSRAM chip select** (`IO_MUX_GPIO15_REG = PERIPHS_IO_MUX_U_PAD_SPICS1`; board
    label "NC/IO15"). Driving it low as a buzzer ground: `rst:0x1a (CPU_LOCKUP)`, then
    `invalid header` / `SPI flash busy` (garbage flash reads). No damage
  - **IO27 as a ground** (strapping pin + RGB LED data): its reset level only selects ROM
    UART printing (`EFUSE_UART_PRINT_CONTROL` 1/2), and this chip's eFuse is 0 = always
    print, so it is ignored. Boot mode is IO28 (BOOT). With the buzzer on IO5/IO27: 10
    resets all booted normally, no chirp (one strap reading `0x1a` vs `0x18`, harmless);
    download mode entered 6/6 (esptool). Its role in download mode on the C5 isn't in
    these files (C6 analogy: needs high) — 6/6 says the buzzer doesn't disturb it
  - new pins, probe: IO5/IO27 loaded 3061 mV (2624–3325, coil switching); alarm on both
    buzzers **50/50 rounds, 0 resets** (110 s). Hub (new firmware, GPIO config buzzer1=5,
    gnd1=27, buzzer2=6, gnd2=8): `t_buzz` played the full alarm, **0 resets** (uptime
    46 → 93 s, serial captured), both buzzers heard (user)
  - the probe gained `w` (Wi-Fi STA from `prov`, RAM-only driver config, no PHY cal stored,
    pings the gateway every 50 ms) and `l R G B` (RGB LED frame on IO27), and accepts
    IO27 only as a ground
  - **tooling**: the CH343 auto-reset circuit resets the chip whenever DTR and RTS differ,
    even briefly — setting them one after the other in pyserial reset the hub twice.
    Leave both at their defaults (equal) to read the serial port without a reset
- [x] **Decision rule** (recorded here with the numbers):
  - loaded voltage ≥ 3.0 V → direct drive OK, any drive strength that's loud enough
  - 2.5–3.0 V → direct drive OK at max drive strength, beeps only (no long tones),
    duty-cycle limited in `gpio.beep`
  - < 2.5 V → too much current for a pin: needs a transistor (BC547/2N2222 + 1 kΩ)
  - applied (2026-11-07): 3.00 V average at drive 3, sticker off → direct drive, drive 3.
    The peaks (dips to 2.42 V) were the open question; 50/50 clean rounds settled it
- [x] Result became Phase 5's defaults (`auto_gpio.c`): allowed pin list, drive strength, and any
      on-time/duty limits `gpio.beep` enforces for this buzzer type. Allowed pins:
      0–10, 23–26, plus **27 as a ground only** (the UI offers only "ground" for it), in
      `auto_gpio.c` since 2026-11-08 (reasons per pin: GPIO map, Hardware section; the first
      list, from the measurements, was 3–6, 8–10, 23, 24, 26); drive strength 3 for
      outputs; a second GPIO driven low works as a buzzer's ground (IO6 + IO8, IO5 + IO27). Tested
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
      commands devices at all (no `zb.set` in Lua, no commands from Home Assistant) —
      only because none of the paired devices can be commanded (see Decisions)
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

### Phase 4b — Subnet router for a remote Home Assistant (not doing, user decision 2026-11-07)
Asked: could the hub advertise the home LAN on the tailnet so a remote HA finds devices
like Ariston and hOn? **Not needed**: both are cloud integrations, so a remote HA reaches
them already. hOn = pyhOn → Haier's hOn cloud. Ariston NET water heaters/boilers = Ariston
cloud (HACS integration). The only local Ariston path is Ariston *air conditioners* via HA's
Midea integration (HA 2026.8, local polling); revisit only if one of those shows up.
The user's Ariston is a water heater (cloud only). Typing device IPs by hand in HA would be
fine for the user, so missing discovery is no blocker if a local-only device comes along.

Findings, in case it comes back:
- **Feasible**: microlink sends `Hostinfo` (4 places in `ml_coord.c`), where
  `RoutableIPs` advertises routes (verify against tailscaled before relying on it). lwIP has
  `CONFIG_LWIP_IP_FORWARD` + `CONFIG_LWIP_IPV4_NAPT` with `ip_napt_enable_netif()` on the
  WireGuard netif; that netif is a /10, so replies route back; wireguard-lwip only checks a
  peer's *source* address
- **Doesn't give discovery**: mDNS/SSDP/broadcast don't cross Tailscale, so the remote HA
  could reach devices by IP but never auto-discover them
- To handle if built: route approval in the admin console (or `autoApprovers`); HA
  Tailscale add-on needs `accept_routes: true` (off by default); prefer per-device /32
  routes (least access, and a /24 clashes if the remote LAN uses the same subnet); drop
  packets arriving on Wi-Fi that aren't for the hub, or LAN hosts could reach the tailnet
  through it; esp-lwip NAPT has no MSS clamping (WireGuard MTU 1280); NAPT table defaults
  to 512 entries (~16 KB); forwarding costs CPU on the lwIP/WireGuard path, so Zigbee
  first: rate-limit

### Phase 5 — Automation (Lua 5.5) (built and mostly verified 2026-11-07; boot-loop guard, RGB LED, new pins, buzzer endurance 2026-11-08; router-off test pending)

**Tests still to do** (user: "tests are last"), in one place:
- [x] `zb.on` firing on a real door change: `t_door` logged `Door open=true` → `false`
  (with `zb.get` agreeing) on 2026-11-07 18:33 and 2026-11-08 00:53
- [x] then a restart: the door's last state must come back (`devices.json` persistence).
  OTA restart 2026-11-08: `t_door` printed `Door open=false` at start, before any report
  (the boot before: `Door open=nil`). Keys stays `nil` until it reports once
- [ ] router off for an hour: triggers, timers, cron and buzzers keep working; `net.on("down")`
  fires; a `retry_for` request made offline goes out when the router is back
  (skipped for now, user 2026-11-08)
- [x] buzzers audible with `gpio.pattern` (the user listens): `t_buzz` on the hub, both
  buzzers (IO5/IO27, IO6/IO8), full 10 s alarm, 0 resets, heard (2026-11-08; see Phase 1b
  follow-up — the first try reset-looped the hub with the old IO5 → GND seating)
- [ ] `gpio.on` with an input
- [x] UI on the phone: Automations layout (Secrets/GPIO moved there, wrapping rows), the
  CodeMirror editor (typing, completion, error line on Save), Settings → Time zone
  (user, 2026-11-08)
- [ ] the Wi-Fi watchdog's reboot gate deferring for a script (needs a 20+ min outage;
  skipped for now, user 2026-11-08)

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
  `gpio.*`, `net.online`/`net.on("up")`, `zb.devices`/`zb.get`, `zb.on` (door changes, see
  the tests above). **Not yet observed:** `net.on("down")`, `retry_for` across an outage,
  `gpio.on`. Last known device state (zone status, on/off, occupancy within 2 s of a
  change; battery/temperature/humidity at most every 5 min) is saved in `devices.json` and
  restored at boot, so `zb.get` and the UI have it right after a restart (verified
  2026-11-08: the door's state was back after an OTA restart, before any report). Side effect:
  the first report after a boot no longer counts as a change unless the value differs
  - `zb.on(device, attr, fn)` — state change triggers
  - `zb.get(device, attr)`. No `zb.set` yet: none of the paired devices can be commanded.
    In principle scripts set devices (user, 2026-11-08); `zb.set` comes with the first
    device that takes commands
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
      example. A buzzer's ground may be a second pin driven low (Automations → GPIO), set as
      "ground for <output>" (built and verified 2026-11-08 with the RGB LED item; the first
      build had no link, only "ground (always low)").
      Defaults from Phase 1b: drive strength 3, 2 s cap per pulse
  - **Network state** (user request 2026-11-07): `net.online()` (Wi-Fi associated with
    an IP, now) and `net.on("up"|"down", fn)`, so a script can change what it does
    when the uplink is gone (e.g. a longer local alarm when Alexa can't be reached)
- [x] GPIO safety — verified 2026-11-07 (buzzer1 IO5, buzzer2 IO6 + ground IO8, config
      left on the hub): pins 28 and 11 refused; a second script using an owned pin stops
      with "GPIO 5 is used by script …"; a script that errored 0.3 s into a 5-round
      pattern left both pins low, the pattern stopped and no owner (checked 1 s after the error)
  - only pins enabled in the UI (Automations → GPIO: name, direction) are usable;
    scripts may refer to them by name (`gpio.write("buzzer", 1)`)
  - pins that are never allowed: SPI flash / PSRAM bus (15–22), USB D−/D+ (13/14), UART0
    console (11/12), BOOT (28); 27 as ground only. Exact list and reasons: "GPIO map of
    this board" (Hardware section), from the C5 datasheet + this board's schematic
  - [x] **the 2026-11-08 allow list is in code** (`auto_gpio.c`, `app.js`, the probe, the
    error message). Verified on the hub: GPIO 0, 1, 2, 7, 25 each configured as an output,
    driven high/low and read back 1/0
    - [ ] the hub still boots normally with something attached to 7 and 25 (nothing on them yet)
  - one script owns a pin at a time. **Claims only while the script loads** (user decision
    2026-11-07): any gpio call at top level claims, `gpio.claim(...)` for pins used only in
    handlers; a handler using an unclaimed pin gets an error. So a conflict is reported at
    Save (the save response carries it: `started: false`), never when the trigger fires
  - **fail-safe**: when a script stops (disabled, error, timeout, OOM, deleted),
    every output it owned goes low and any beep/tone stops — a crashed script
    must never leave a buzzer screaming
  - the UI GPIO page shows each enabled pin's state and owning script
- [x] **RGB LED on IO27** (user request 2026-11-08; built and verified 2026-11-08). The board's LED between the USB ports
      is an addressable WS2812-type LED on IO27 (WS2812B-0807 per the schematic). **It takes
      R, G, B order**, not the usual WS2812B G, R, B: GRB showed red for green (user). Values
      go out as given, no white balance (equal values look slightly bluish; user decision:
      scripts balance it themselves). Verified on the hub: red → green → blue → off, mixed
      colours, fail-safe off when the owning script errors, off after a crash reboot (was
      red), a colour requested during a buzzer pattern stays pending and shows when it
      ends, the "ground for" link (gnd1 → buzzer1, gnd2 → buzzer2) and its validation. It latches its colour
      until the next frame or power-off. IO27 is also buzzer 1's ground (+ IO5, − IO27), so
      they share the pin. Design (user, 2026-11-08):
  - **Lua never sees IO27 as a pin.** Ground is a setting (Automations → GPIO, pin 27 =
    "ground"), held low by C from boot; `gpio.write(27, …)` is an error like any ground pin.
    The LED is its own API with no pin number: `led.set(r, g, b)`, `led.off()`
  - **`led.claim()`**, same rules as GPIO claims: one script owns the LED; any `led` call at
    top level claims it, `led.claim()` for a script that uses the LED only in handlers; a
    second script gets a conflict error at Save, and a handler can't claim it later
  - **The LED is not between IO27 and ground**: WS2812 = power + ground from the board,
    data-in on IO27 (draws ~nothing). As a ground IO27 is steady low = "no data", the LED
    keeps its colour. A frame = 24 high/low pulses (~30 µs, IO27 not a ground) + ≥ 50 µs low
    (latch, a ground again) ≈ 80 µs
  - **C arbitrates the pin** (nothing hard-coded to IO5, any user's wiring):
    - a ground pin's setting names the output(s) it is the ground for (e.g. pin 27 = "ground
      for buzzer1"). For a frame, C needs those outputs low, switches them to **input**
      (not driven, no pull) for the ~80 µs, sends the frame, drives them low again. With
      the + leg undriven no current can flow through the buzzer either way, whatever IO27
      does (today's LED test ran exactly like that: IO5 was an input). Without the
      release, IO5 low + IO27 pulsing high = the buzzer in reverse for < 1 µs per pulse
      (unmeasured)
    - fallback when a ground pin names no output: treat **every** configured output as
      possibly wired to it — all must be low, all released for the frame. Never wrong,
      only slower (an unrelated output held high, e.g. a relay, delays the LED)
    - an output that has to be low but is high (pattern, beep, tone, `gpio.write(…, 1)`):
      the colour is stored and sent as soon as they are all low. `led.set` never blocks
  - **Fail-safe in C**: when the owning script stops (error, timeout, OOM, disabled,
    deleted), C sends "off" in the same path that drives its outputs low and stops its
    buzzers (deferred to the end of another script's pattern if one is playing). C also
    sends "off" once at every boot: the LED keeps its colour through a reset, only power-off
    clears it, so after a crash or watchdog reboot it would stay on. If the hub itself
    crashes nothing can send a frame; the boot-time "off" clears it ~1 s later
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
- [x] **Boot-loop guard** (user, 2026-11-08; built 2026-11-08). A script must never be able to keep the hub
      in a reset loop — Zigbee is the top priority. Found the hard way: `t_buzz` played at
      start, a loose buzzer reset the chip (`RTC_PWR_GLITCH`) ~1.7 s into every boot, and
      `s_rtc_run` didn't catch it because the pattern runs in C after the script's top
      level has returned (Phase 5 findings 2026-11-08). Two layers:
  - **Buzzer marker**: whenever a pattern, beep or tone is playing, a marker in RTC memory
    names the owning script(s); cleared when the last one stops. At boot, if the reset was
    abnormal and the marker is set, that script is **disabled** (persisted, like
    `s_rtc_run`) with the reason shown in the UI: "the hub reset (power glitch) while this
    script's buzzer was playing". Abnormal = power glitch, CPU lockup, brownout, panic,
    interrupt/task watchdog. Normal = power-on, the RST button/serial reset, `esp_restart()`
    (OTA, planned reboots, the Wi-Fi watchdog's reboot)
    - first check whether RTC memory survives an `RTC_PWR_GLITCH` reset on the C5 (the
      reset wipes more than a panic does). If it doesn't, the marker goes to NVS, written
      by the automation task when a pattern starts (alarms are rare, so the flash wear is
      negligible) — never from the esp_timer callback
  - **Safe mode**: a counter of abnormal resets that happened less than 60 s after boot
    (RTC memory, or NVS if RTC doesn't survive), cleared once the hub has run 60 s. At 3 in
    a row the engine **starts no scripts** this boot (nothing disabled, nothing deleted):
    UI banner "Safe mode: the hub reset 3 times in a row soon after starting; scripts were
    not started", with "Start scripts" and the last reset reasons. Zigbee, Wi-Fi, web and
    Tailscale start as always — they already come up before scripts
  - neither layer changes Zigbee's boot order or blocks it: both run inside the engine's
    start, after the device registry is loaded
  - **Tests**, each verified from the serial log and the UI:
    - [x] RTC marker survives a panic: `rtc_kept: true` after every deliberate crash
      (test build: `idf.py -DHUB_TEST_HOOKS=1`, `POST /api/debug/crash`; installed by OTA
      and crashed only after the trial was confirmed)
    - [ ] RTC survives a real `RTC_PWR_GLITCH`: unknown until one happens — every boot logs
      "RTC memory kept/not kept" with the reset reason (the canary). The safe-mode counter
      is in NVS, so safe mode doesn't depend on it
    - [x] panic while a pattern plays → next boot: that script disabled with "the hub reset
      (crash) while this script's buzzer was playing…", LED off (was red), the other
      script running, Tailscale and Zigbee back
    - [ ] the real case: during an alarm the user wiggles a buzzer until the chip resets →
      one reset, script disabled, no loop
    - [x] safe mode: crash 5 s into each boot (`{"boots": 3}`) → counter 1 → 2 → 3, then "SAFE
      MODE, scripts not started" (the enabled `t_door` stayed stopped), Wi-Fi + Tailscale +
      API up, API `boot.safe_mode: true`; `POST /api/auto/safe` (the banner's "Start
      scripts") → safe mode off, `t_door` running. The UI banner itself: not looked at yet
    - [x] normal reboots never disable anything: Settings → Restart and an RST pulse (reports
      power-on), each during a playing alarm → script still enabled, reset reason "normal".
      OTA restarts use the same `esp_restart()` path as Settings → Restart
- [x] **Buzzer endurance on the hub** (user, 2026-11-08), buzzers held by the case (legs 2
      holes apart, IO5/IO27 + IO6/IO8), serial captured:
  - [x] 50 alarm rounds back to back (the Phase 1b standard): 0 resets — see the next item
  - [x] the same with Zigbee traffic — **passed 2026-11-08 with three buzzers** (buzzer 3 =
    + IO2 / − IO0 added; GPIO config `buzzer3`=2, `gnd3`=0 "ground for buzzer3"), a harsher
    pattern than the alarm: 2 s rounds of a 30 ms A→B→C chase, all three stuttering 25/25 ms,
    rotating pairs 50 ms, all three on 400 ms, 60 ms off; 50 rounds (100 s). The user flipped
    the Keys sensor during the first ~65 s: **62 changes received, strict open/closed
    alternation, 0 repeats** (each also seen by `t_door`); **0 resets** (uptime 849 → 992 s
    unbroken, 150 s serial capture with no WARN/ERROR). The first item (50 rounds, 0 resets)
    is covered by this run. Heap afterwards: 69 KB internal free, lowest since boot 30 KB
    (Phase 5 recorded 37–40; not traced)
  - [x] the same with Wi-Fi load — **passed 2026-11-08**: the 3-buzzer chime, 50 rounds,
    while the dev machine downloaded `cm.js` over the tailnet in a loop (163 × 120 KB = 19.6 MB,
    0 failed) and pinged the LAN IP every 50 ms (2471/2471, avg 21.6 ms, max 93 ms): **0
    resets** (uptime unbroken), 0 Wi-Fi disconnects. Together with the Zigbee run above this
    settles Phase 1b's open question: neither radio makes the buzzers reset the hub
- [ ] Test: unplug the router for an hour — every trigger and timer still fires,
      verified from the serial log
- [x] UI editor (verified by the user, 2026-11-08): paste/edit,
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
- [~] Built-in examples: `examples/*.lua` in the repo (since 2026-11-07, user
      request: a clear location), compiled into `app.js` at build time, offered in the editor;
      none is stored on the hub until saved (door opens → buzzer + Alexa URL with `retry_for`; weekday-morning
      `cron` that logs which doors are still open)
  - [x] `door-alarm.lua` (door names changed to Door/Keys) verified 2026-11-08: Keys opened →
    alarm on buzzers 1 + 2 at once, routine URL (virtualsmarthome.xyz URL Routine Trigger,
    `response=json`, stored as secret `alexa_alarm_url`) → HTTP 200 in 1.5–2.7 s from the hub
    (0.22 s from the dev machine: the hub pays a full TLS handshake), Alexa ran the routine
    (user). Each opening restarts the alarm and calls the URL again (3 calls in 13 s) — 5b
    needs one call per alarm. The JSON reply carries `"triggerActivationStatus": "success"`.
    The trigger is a doorbell-type device in Alexa, so Alexa also announces "<trigger> ha
    rilevato un visitatore"
    Changed the same day (user): the alarm plays while a door is open (pattern capped at
    the engine's 10 min, now on all three buzzers `buzzer1`/`buzzer2`/`buzzer3` with the
    3-buzzer pattern) and `gpio.stop()` ends it when the last open door closes; Alexa is
    called once per alarm start, not on a second door opening. Verified: 4 open/close cycles
    on Keys (open 1–5 s), alarm off the moment Keys closed, each start → HTTP 200 in
    1.6–1.9 s; works "perfectly" (user). Not exercised: two doors at once, an alarm past
    10 s. A door reopened right after closing starts a new alarm and a new Alexa call
    (two calls 2 s apart) — 5b should hold off repeat calls
  - [ ] `morning-check.lua` not run yet

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

#### Phase 5 findings (2026-11-08)
- **A buzzer script reset-looped the hub** (`t_buzz`, saved enabled, played at start):
  `RTC_PWR_GLITCH` ~1.7 s into every boot. The crash guard (`s_rtc_run`, script disabled at
  the next boot) didn't catch it: the script's top level had already returned, the pattern
  runs in C. Cause: loose buzzer legs (Phase 1b follow-up); fixed by the new pins.
  The boot-loop guard below now stops such a loop (built and tested 2026-11-08)
- **IO15 was on the allowed pin list** and crashes the chip (PSRAM chip select): removed
  from `auto_gpio.c`, the UI, the probe, README
- **The GPIO config had no "ground for …" link**: the plan said "ground for …" on
  2026-11-07, the build made it "ground (always low)" with nothing linked, and the GPIO
  item was ticked anyway. Built 2026-11-08 with the RGB LED (config field `for`, UI choice)
- **Two failed OTA trials** (2026-11-08), both rolled back by the bootloader as designed:
  1. stack overflow on the main task (3.5 KB): a 2 KB config buffer on its stack in
     `agpio_init` → heap corruption → crash. Buffers moved to the heap
  2. `ESP_ERR_HTTPD_HANDLERS_FULL`: the URL-handler limit (40) was reached → abort at boot.
     Raised to 56 (~40 in use). Found only by capturing the console during the trial boot
     (the core dump's backtrace stopped at `abort`)
- **Two crashed trial boots in a row suspend Tailscale** (its own crash guard, `ts.c`):
  after the rollback the hub stayed off the tailnet, so in tailnet-only mode the UI was
  unreachable until a clean (power-on) reset or BOOT held 10 s (LAN access). **Kept as is**
  (user decision 2026-11-08): a rolled-back OTA trial counts toward Tailscale's crash guard
  like any other crash
- **`espefuse` leaves the chip in download mode**: the hub stays down until reset (RTS
  pulse). The CH343 auto-reset resets the chip whenever DTR and RTS differ, even briefly

### Phase 5b — The user's own alarm script (user request 2026-11-07)
Built on Phase 5, before Home Assistant: it runs entirely on the hub, so it needs
nothing from Phase 6.
- [x] **The script is the keys alarm** (`examples/keys-alarm.lua`, installed on the hub as
      `keys-alarm` 2026-11-08; replaces the `away`-based version, which missed lockouts even
      with one person). Door + Keys tap + Wi-Fi motion, nothing remembered between openings:
      - Door opens ≤ 60 s after a Keys tap (`KEYS_WINDOW`; M1 accepted at 60 s): silent, and
        the tap is used up
      - Door opens and `motion.moved(62, 2)` is false (nothing moved in the minute before,
        the last 2 s left out as the opening itself): silent, coming home
      - anything else, `nil` (no motion signal) included: alarm — 3 buzzers + Alexa at once,
        10 s (5 rounds), stopped by a Keys tap or the Door closing; a tap changes nothing else.
        Alexa called the moment the alarm starts (F12), but not again within 30 s: calling it
        on every alarm was "too eager" (user, 2026-11-08), as the earlier script's 30 s did
      - `STILL_S` = 60 s and `GAP_S` = 2 s are Claude's choice, not measured (F10 is still the
        user's): longer = fewer silent openings (safer), more homecoming alarms when someone
        moved inside shortly before
- [x] Lua API for it: `motion.moving()` and `motion.moved(from_s, to_s)` (10 min of history,
      one flag per 500 ms; movement anywhere in the window → true even where part of it
      wasn't seen; otherwise any unseen part → nil). Verified with a throwaway script:
      true/false/nil as expected, bad windows raise an error
- [x] **Buzzers always**, Alexa in addition (user decision 2026-11-08). The routine URL is a
      virtualsmarthome.xyz URL Routine Trigger (`alexa_alarm_url`, `response=json`); its reply
      `"triggerActivationStatus": "success"` means the service accepted it, **not** that an
      Echo played. Alexa also announces the trigger as a doorbell visitor unless that device's
      announcements are turned off in the Alexa app
- [x] User, 2026-11-08: open without a tap → alarm; tap then open → silent. (The old script
      did the same; Alexa was called on every alarm, so the 30 s cooldown went back in)
- [x] What the old script got wrong, verified by the user on hardware (2026-11-08): come home
      after more than a minute with nobody inside → silent; tap to stop an alarm, close, later
      open without a tap → alarm; tap, open, don't go out, close, later open without a tap →
      alarm
- [x] The alarm stopped by closing the door, and by a tap (user, 2026-11-08)
- [ ] Same with the router unplugged: every opening without a tap sounds (no motion signal),
      Alexa once the router is back

#### Keys alarm (`examples/keys-alarm.lua`): failure decisions (user, 2026-11-08)
Setup: "Door" contact sensor; "Keys" contact sensor on the inside of the door, a magnet on
the one key; tapping the key on "Keys" = "I'm leaving with it". No hardware on the market
tracks where the key is (user), so the hub never knows it: every wrong guess must be an
alarm, never silence. No procedures: nobody does anything except tap when leaving with the
key. Design under review: a Door opening is silent only after a Keys tap within the window,
or when the house was still (Wi-Fi sensing, CSI) for a set time before it (coming home);
anything else sounds the alarm. Every alarm is the same (user): full alarm (3 buzzers,
Alexa at once) for 10 s, stopped earlier by a Keys tap or the door closing; the tap only
stops it and changes nothing else.
- Accepted:
  - M1: tap, key put down, out within the window (window may be 60 s)
  - M6: a buzzer unplugged or broken
  - M7: door propped open, no Door event (closing it is loud)
  - F1: coming home while someone else is home → alarm
  - F6: leaving with the key without tapping → alarm until a tap
  - F7: Wi-Fi down → every homecoming alarms
  - F8: hub restarted recently → homecomings alarm
  - F9: something moving in the empty house → homecoming alarms
- Accepted only on conditions:
  - F2: B leaves while A has the key → alarm: only if it stops when the door closes
    **and** the hub guarantees A has the key and it isn't in the house
  - F3 (A leaves the key for B, B at home), F4 (delivery, looking out), F5 (second person
    let in by the first) → alarm: only if the tap that silences it causes no further problem
- Rejected:
  - F12: delaying the Alexa call. Alexa is called the moment the alarm starts
- Open:
  - M2: Wi-Fi sensing misses a walk to the door → decided by the sensing test
  - F10: how long the house must be still before an opening counts as coming home
  - F11: the person coming home seen on the landing before opening → alarm
- Accepted as long as a natural action resets everything to normal (rule for every
  failure, user): M3 (still by the door for long, out within 3 s of moving), M4 (Door
  report lost, sensor offline or flat), M5 (hub down or script disabled). The design keeps
  nothing from one opening to the next (no `away`, no key location), so once the door is
  closed the next opening is judged fresh: closing the door is the reset

#### Wi-Fi motion sensing (`components/motion`; built and installed 2026-11-08)
Settings → Motion shows **Moving** / **Not moving**; two buttons are the only calibration:
"Not moving" (stay still 30 s → still level = p99) and "Moving" (move 15 s → moving level =
p25); threshold = geometric mean of the two (still only: 2.5 × still; neither: 800), kept in
NVS. "Moving" = a 500 ms score above the threshold within the last 2 s. Not used by the
alarm or scripts yet. A first version logged scores and events to LittleFS for analysis;
removed at the user's request once the live test below was convincing.
- How: the hub pings its gateway every 50 ms; the CSI of every frame addressed to the hub is
  scored every 500 ms (mean per-subcarrier std dev of the amplitude normalised per frame,
  x10000). It sees movement anywhere along the router–hub path, both ends included
- CSI on the C5, measured: with only `acquire_csi_legacy` + `force_lltf` the driver gave CSI for
  the router's beacons only (format 1, ~6/s). The ping replies (HE SU, format 4) needed the HE
  options too (ht20/ht40/vht/su/mu/dcm/beamformed/stbc all set; which one did it is not isolated).
  The replies' source MAC is the router's LAN MAC, not the BSSID: the filter is "addressed to
  the hub". With `force_lltf` both formats give 106 bytes (53 subcarriers, 8-bit I/Q). The
  score uses one format, the one most frames came in over the last 10 s (format 4 here):
  beacons are sent differently and would mix two channel shapes. 23–33 frames/s scored
- Live test (user at the desk, hub placed by the user): sitting still 160–240 with short jumps
  to 470; walking to the door 1,360–2,960; standing at the door 230–590; walking back
  1,310–2,670. Waving in front of the router 1,270–3,760, in front of the hub 1,570–4,340,
  still in between 150–290
- **RAM**: `CONFIG_ESP_WIFI_CSI_ENABLED` alone took the lowest internal free from 30 KB to
  17.9 KB (sensing off) / 14.7 KB (on): the driver reserves per static RX buffer at boot.
  Lever 2 pulled: static RX buffers 16 → 8 → **69 KB free, lowest 32.9 KB**, sensing on
- Load with sensing on (logging version): 38 downloads of `cm.js` over the tailnet in 30 s
  (4.5 MB, 0 failed), LAN ping every 100 ms 300/300 (avg 21.5 ms, as before), 0 Wi-Fi
  disconnects, Zigbee up
- [x] moving / not moving checked by the user with the default threshold 800 (no calibration
  pressed): "insanely accurate" (2026-11-08)
- [x] both calibration buttons used by the user: learned threshold **798** (geometric mean of
  the two levels; the individual levels aren't exposed), next to the hand-picked 800

### Phase 6 — Home Assistant integration (ESPHome native API)
**One entity set, additive (user decision 2026-11-08; replaces the two modes of
2026-11-07).** With the integration on, Home Assistant always sees every Zigbee device as
its own sub-device of the hub, plus the hub's own entities, plus whatever enabled scripts
publish. Enabling or disabling scripts never hides a device. The earlier design switched
between "devices mode" (no script enabled) and "smart-device mode" (the hub as one device,
Zigbee devices hidden): dropped because enabling a first script would have broken every HA
dashboard, automation and history built on the devices; clutter is HA's to hide. Today
nothing commands devices, so HA and scripts can't conflict; once devices can be set
(`zb.set`, commands from HA), both may command the same device, and the last command wins —
the same as two people at two switches.

- [ ] **Setting** "Home Assistant integration", **off by default**, in the UI
      (Settings) and as `flash.sh --ha` at provisioning. Off → the API server
      never starts, port 6053 closed, nothing advertised
- [ ] **Script entities (user decision 2026-11-07)**, in addition to the devices: one
      switch per script (enable/disable); entities a script publishes through a small Lua
      API (e.g. `ha.sensor(name, value)`, `ha.binary(name, on)`, `ha.event(name)`), so a
      script can expose "alarm armed", "doors open: 2", "alarm triggered". A disabled
      script's entities stay, unavailable (history kept); deleting the script removes them
- [ ] Entity set changes (a device paired or removed, a script publishing a new entity, a
      script deleted): the API server keeps running; the hub closes its API clients (each
      client task closes its own socket — lesson 8) and HA reconnects and lists the new
      set by itself. Entity keys come from IEEE address + attribute (devices), script name
      + entity name (scripts) and fixed names (hub), so HA entity IDs and history survive
      every change
- [ ] ESPHome native API on TCP 6053, **Noise encryption mandatory** (no plaintext).
      Key generated by the board the first time the setting is turned on (or by
      `flash.sh --ha`, stored in `.esp/secrets/` and written to `prov`); shown
      in the UI for pasting into HA, regenerable
- [ ] mDNS `_esphomelib._tcp` so HA discovers the hub automatically on the LAN.
      Over the tailnet (HA elsewhere) mDNS doesn't cross: add it in HA by
      MagicDNS name, `zigbee-hub.<tailnet>.ts.net:6053`. In tailnet-only access mode
      the API accepts only tailnet connections (same check as the web server) and
      nothing is advertised
- [ ] **Sub-devices** (HA/ESPHome 2025.7+): hub = main device, each
      Zigbee device = sub-device with its own name/area/manufacturer/model from the
      interview. `device_id` field numbers per entity type are in the reference
      repo's CLAUDE.md ("Protobuf device_id Field Numbers") — verify against the
      current `api.proto` before relying on them
- [ ] Capability → entity mapping (same capabilities as the UI uses):
      sensors → sensor with device_class, IAS zone → binary_sensor,
      battery/LQI/last seen → diagnostic entities. Read-only for now: no paired device
      takes commands. Commandable devices get switch/light/... entities when they come
- [ ] Hub entities: permit-join button (pairing works from HA too),
      Zigbee channel, device count, Wi-Fi RSSI, uptime
- [ ] **The hub is a motion sensor itself** (user, 2026-11-08): Wi-Fi motion sensing
      (`components/motion`, Phase 5b) exposed as a `binary_sensor` with
      device_class `motion` on the hub's own device (Moving = on), with "no signal" (Wi-Fi
      down, no frames) reported as unavailable, not as "not moving"
- [ ] Wire-format safety: every length off the socket bounded before use
      (reference had a pre-auth overflow on the hello frame — lesson 9)
- [ ] Entity table in PSRAM (reference: 54 KB, moving it freed half the internal heap)
- [ ] The web UI is unchanged by the integration (devices, enrollment, network, status)
- [ ] When the setting is off, the ESPHome API code costs no runtime RAM (server
      task and entity table only allocated when it's on)
- [ ] Verified: HA discovers the hub, adopts with the key, shows each paired device as
      a sub-device and the hub's own entities; enabling a script that publishes entities
      adds them and leaves every device and its history in place; disabling it leaves
      its entities unavailable

### Phase 7 — Hardening
- [ ] Soak test 24 h+ (reference repo's worst bugs needed 9 h of uptime)
- [ ] Internal heap low-water mark measured: Zigbee + Tailscale + UI client + 3 scripts,
      and the same with HA connected
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
With Wi-Fi CSI compiled in (2026-11-08) the lowest fell to 14.7 KB; lever 2 pulled (static RX
buffers 16 → 8): **69 KB free, lowest 32.9 KB** with motion sensing on.

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
- **HA entities are additive** (opt-in setting on; 2026-11-08, replaces the two modes of
  2026-11-07): every Zigbee device always appears as its own HA device; the hub's own
  entities and whatever enabled scripts publish come on top. Scripts never hide devices.
- **Lua** for automations (see Phase 5).
- **Script editor = CodeMirror 6**, prebuilt and committed (2026-11-07; Monaco rejected:
  no mobile support, several MB).
- **GPIO pins are claimed only while a script loads** (2026-11-07), so conflicts show at
  Save. Secrets and GPIO settings live on the Automations page (2026-11-07).
- **Commanding Zigbee devices: not yet, not never** (2026-11-07, clarified 2026-11-08): in
  principle scripts (and HA) set devices; there's no `zb.set` and no HA commands only
  because none of the paired devices (contact sensors) can be commanded.
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
