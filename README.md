# ESP32-C5 Local Zigbee Hub

> **Work in progress.** This README describes the finished product. Working today:
> 5 GHz-only Wi-Fi with backup network and recovery, provisioning via `flash.sh`,
> the web UI (login, live status, Wi-Fi settings, devices), over-the-air updates
> with rollback, and the Zigbee coordinator with enrollment of battery sensors
> (tested with contact sensors). Tailscale, automations and Home Assistant are
> next. [`CLAUDE.md`](CLAUDE.md) tracks the real status, phase by phase.

A Zigbee 3.0 coordinator, automation engine and web UI on a single ESP32-C5,
reachable from anywhere over your tailnet. No cloud, no Home Assistant, no MQTT
broker, no Raspberry Pi.

- **Zigbee owns 2.4 GHz.** The board's Wi-Fi runs **only on 5 GHz**; 2.4 GHz Wi-Fi
  is disabled in firmware, so the uplink never competes with your Zigbee devices.
- **Automations run on the board.** Paste a Lua script into the UI and it runs
  locally. It keeps running when Wi-Fi is down.
- **One command to set up.** `./flash.sh` builds the firmware and flashes it with
  the Wi-Fi credentials of the machine you're flashing from.
- **On your tailnet.** The board joins Tailscale as `zigbee-hub`, so the UI works
  from your phone on cellular just like it does at home.
- **Made for battery sensors.** Contact, motion, leak, smoke, temperature and
  humidity sensors. Mains-powered Zigbee devices (plugs, bulbs, other routers) are
  untested and not what this hub is built for.
- **Optionally, native Home Assistant devices.** Turn on the Home Assistant
  integration, and whenever no local automation is enabled, every Zigbee device
  appears in Home Assistant as a native ESPHome device. No MQTT, no ZHA, no
  zigbee2mqtt.

## Design priority

**Zigbee and local automations are the part that must never stop.** Wi-Fi and
Tailscale are conveniences on top:

- Zigbee and automations start before Wi-Fi and don't wait for it.
- Losing Wi-Fi doesn't interrupt anything. The board reconnects on its own while
  your automations keep firing. If the Wi-Fi chip itself gets stuck, the board
  reboots as a last resort, but only at a moment when no timer is about to fire
  and nothing is pairing or updating, and pending timers survive the reboot.
- When the radios contend, Zigbee wins.
- Scripts that call out to the network queue their requests and move on; a dead
  uplink can't stall an automation.

## Hardware

**ESP32-C5 with 16 MB flash and 8 MB PSRAM** (e.g. a C5 module with PSRAM on any
USB-UART dev board). No wiring needed, except for optional buzzers or other
GPIO devices your scripts drive.

| | why |
|---|---|
| C5 | dual-band Wi-Fi: uplink on 5 GHz, Zigbee alone on 2.4 GHz. C6/H2 are 2.4 GHz only |
| 8 MB PSRAM | Tailscale's map buffers, the device registry and script heaps live there |
| 16 MB flash | two 4 MB OTA slots, Zigbee storage, and ~7 MB for devices and scripts |

## Quick start

Everything (ESP-IDF, compiler, Python) installs **inside this folder**, in `.esp/`.
Nothing goes into your home directory or system packages. You only need `git`,
`cmake`, `ninja`, `uv` and NetworkManager (`nmcli`).

```bash
# 1. Plug the board in, then:
./flash.sh --ts-authkey tskey-auth-XXXXXXXX
```

`flash.sh`:

1. reads the Wi-Fi network this machine is connected to (via `nmcli`)
2. **refuses** if that network has no 5 GHz access point, and warns if it's on a
   DFS channel (52–144); the C5 can't handle radar events, so use 36–48
3. generates an admin password on first run and prints it
4. builds the firmware, writes your credentials into a separate provisioning
   partition, and flashes both
5. watches the board boot and prints where to find it:

```
✓ Connected to MyWiFi on 5 GHz, channel 44, -47 dBm
✓ Zigbee network up on channel 25 (PAN 0x7A3C)
✓ Tailscale: zigbee-hub.<tailnet>.ts.net (100.101.12.7)

  Web UI:   http://zigbee-hub.local/   (LAN)
            http://zigbee-hub/         (tailnet)
  Password: 7f3k-qp9x-m2va
```

The Tailscale auth key is optional at flash time; you can paste it in the UI later.
Use a reusable, pre-approved key from the
[Tailscale admin console](https://login.tailscale.com/admin/settings/keys).

### Other flash options

| | |
|---|---|
| `./flash.sh --prov-only` | re-write Wi-Fi/Tailscale credentials without rebuilding. The Zigbee network and your devices are untouched |
| `./flash.sh --hostname kitchen-hub` | different hostname on the LAN and the tailnet |
| `./flash.sh --ssid X --psk Y` | provision a network other than the one you're on |
| `./flash.sh --monitor` | stay attached to the serial log after flashing |
| `./flash.sh --ha` | turn on the Home Assistant integration and print its encryption key |
| `./flash.sh --ota` | update over the network instead of USB (see below) |
| `./flash.sh --gen-signing-key` | create a key so the board only accepts updates you signed |

Credentials live in their own flash partition. Re-provisioning never wipes the
Zigbee network, so you never have to re-pair devices because the Wi-Fi password
changed. `flash.sh` remembers your backup network, hostname and Tailscale key
between runs.

### Backup Wi-Fi

The hub stores two networks, a primary and a backup. If the one it's on has been
unreachable for 2 minutes, it tries the other.

That makes it possible to **move the hub to a new router without touching it**:

1. In the UI (or with `./flash.sh --backup-ssid … --backup-psk …`), set the
   backup to your phone's hotspot, and switch to it.
2. The hub is now online through your phone. Over Tailscale, enter the new
   router's details as the primary.
3. Switch back, or just turn the hotspot off: within 2 minutes the hub is on the
   new primary.

Every switch made from the UI is on trial: if the new network doesn't give the
hub an address within 60 seconds, it goes back to the previous one, so a typo
can't strand it. The hotspot must be on **5 GHz** (iPhone: turn off
"Maximize Compatibility"; Android: pick the 5 GHz band).

## The web UI

Log in with the admin password. The UI updates live over a WebSocket.

### Devices

Every paired sensor with its live state: contact, motion, water leak, smoke,
temperature, humidity and battery. Each device shows signal strength, last seen
and availability. Rename, re-interview or remove them. The UI only shows state;
there are no switches in it.

### Enroll

Click **Scan for devices** and the hub opens the Zigbee network for two minutes
(adjustable), with a countdown. Put your device in pairing mode. For most
battery sensors, that just means inserting the battery: a no-name reed sensor
straight out of the bag joins, shows up in the list within a second, and reports
open/closed before you've closed the battery cover.

Battery sensors stay awake for only a few seconds after joining, so the hub
interviews them immediately. It also does the things cheap sensors silently
depend on: the attribute read Tuya devices expect right after joining, and IAS
Zone enrollment, without which a contact sensor joins but never reports. If a
sensor falls asleep mid-interview, the hub finishes the interview the next time
the sensor checks in.

New devices appear in the list as soon as they announce themselves, marked
"interviewing" until the interview is done. Give it a name and it's ready, both in
the Devices page and in your scripts.

### Automations

Paste Lua, press **Save**. The script is syntax-checked before it's stored, then
starts immediately and again after every boot.

```lua
-- Front door left open for 5 minutes: beep and send a notification
local door_timer

zb.on("front_door", "contact", function(open)
  if open then
    door_timer = timer.after(300, function()
      gpio.beep("buzzer", {on = 200, off = 200, times = 5})
      http.post(secret("ntfy_url"), "The front door has been open for 5 minutes")
    end)
  elseif door_timer then
    door_timer:cancel()
  end
end)
```

```lua
-- Leak sensor: sound the buzzer at once, then notify (retried until Wi-Fi is back)
zb.on("laundry_leak", "water_leak", function(wet)
  if wet then
    gpio.beep("buzzer", {on = 500, off = 500, times = 20})
    http.post(secret("ntfy_url"), "Water leak in the laundry room!", {retry_for = 3600})
  end
end)
```

```lua
-- Weekday mornings at 7: report sensors with a low battery
cron("0 7 * * 1-5", function()
  for _, name in ipairs({"front_door", "laundry_leak"}) do
    local b = zb.get(name, "battery")
    if b and b < 20 then http.post(secret("ntfy_url"), name .. " battery at " .. b .. "%") end
  end
end)
```

#### Script API

| | |
|---|---|
| `zb.on(device, attribute, fn)` | call `fn(value, old_value)` when an attribute changes |
| `zb.set(device, table)` | send a command to a device that accepts one |
| `zb.get(device, attribute)` | current value |
| `timer.after(s, fn)` / `timer.every(s, fn)` | one-shot / repeating timers, return a handle with `:cancel()` |
| `cron(expr, fn)` | standard 5-field cron, local time |
| `state.get(key)` / `state.put(key, value)` | small persistent key-value store, survives reboots |
| `http.request{url=, method=, headers=, body=/json=, retry_for=, on_done=}` | HTTP(S) request; never blocks the script, the result arrives in `on_done(status, body)` |
| `http.get(url, opts)` / `http.post(url, body, opts)` | shorthands for the above |
| `secret(name)` | a value stored under **Settings → Secrets** (tokens, webhook URLs), so it never appears in script text |
| `log(...)` | shows up in the script's live log in the UI |
| `gpio.write(pin, v)` / `gpio.read(pin)` | drive or read one of the board's pins |
| `gpio.on(pin, edge, fn)` | call `fn` on a rising/falling edge (debounced) |
| `gpio.beep(pin, ms)` / `gpio.beep(pin, {on=, off=, times=})` | sound an **active** buzzer, without blocking the script |
| `gpio.tone(pin, hz, ms)` | play a tone on a **passive** buzzer |

#### Buzzers and other GPIO

Enable a pin under **Settings → GPIO**, give it a name, and scripts can use it:

```lua
-- Door left open for 2 minutes: beep three times, every minute, until it closes
local nag
zb.on("front_door", "contact", function(closed)
  if closed then
    if nag then nag:cancel(); nag = nil end
  else
    nag = timer.after(120, function()
      nag = timer.every(60, function() gpio.beep("buzzer", {on = 150, off = 100, times = 3}) end)
    end)
  end
end)
```

Only pins you've enabled are usable. Pins the board needs for itself (flash, PSRAM,
USB, the serial console, boot-mode pins) can't be enabled at all. Each pin
belongs to one script at a time. If a script stops for any reason (disabled,
error, out of memory), its pins go low and any beep stops, so a crashed script
never leaves a buzzer sounding.

**Wiring:** a pin can safely supply about 20 mA at 3.3 V. Many 3 V buzzers draw
around that, or more. Drive them through a small NPN transistor or logic-level
MOSFET, and add a flyback diode across magnetic buzzers.

#### Calling other services: Alexa, webhooks

Scripts can make HTTP and HTTPS requests. Certificates are verified against the
usual public authorities. The request goes out in the background, so a slow or
offline service never delays the rest of the script; the local buzzer still
sounds at once.

Alexa has no webhook of its own, but third-party skills (routine triggers,
announcement services) give you an HTTPS URL that runs an Alexa routine when
called, for example one that plays an alarm on every Echo in the house. Store that
URL under **Settings → Secrets** as `alexa_alarm`, then:

```lua
-- Water leak: buzzer here, alarm on the Echos, and keep trying for 10 minutes
-- if the internet happens to be down
zb.on("laundry_leak", "water_leak", function(wet)
  if not wet then return end
  gpio.beep("buzzer", { on = 500, off = 300, times = 20 })
  http.get(secret("alexa_alarm"), {
    retry_for = 600,
    on_done = function(status)
      if status ~= 200 then log("Alexa alarm failed, status " .. status) end
    end,
  })
end)
```

`retry_for` is what makes it dependable: without it a request made while offline
is dropped; with it, the hub keeps retrying with backoff until the deadline.
Requests are limited to 10 per minute per script, and responses are capped at
16 KB.

#### Safety and limits

Each script runs in its own sandbox with a memory cap (256 KB by default) and an
instruction limit. A script that crashes, loops forever or runs out of memory is
stopped and its error is shown with the line number; the other scripts keep
running. Scripts have no filesystem access and can't load modules or bytecode.

**Without Wi-Fi**: device triggers and timers always work. Cron needs the time of
day: it is synced over the internet when online and kept by the chip's clock
through outages. If the board boots with no uplink and has never known the time,
cron jobs show "clock unset" instead of firing at the wrong moment.

### Home Assistant (optional)

Off by default. Turn it on under **Settings → Home Assistant integration** (or with
`flash.sh --ha`), copy the encryption key it shows, and Home Assistant discovers
the hub on its own (**Settings → Devices & services → ESPHome**). Paste the key
when asked.

The hub appears as one device, and every paired Zigbee device appears under it
as its own device, with its name, area, manufacturer and model:

| Zigbee device | In Home Assistant |
|---|---|
| temperature/humidity sensor | sensors with the right device class |
| contact, motion, leak, smoke sensor | binary sensors |
| any device | battery, link quality, last seen (diagnostic) |

The hub itself adds a **Permit join** button, so you can pair new devices from
Home Assistant too.

**One brain at a time.** The integration is active only while **no automation
script is enabled**:

- Enable a script, and the hub becomes the brain: it confirms first, then
  disconnects Home Assistant, where the devices show as unavailable.
- Disable all scripts, and Home Assistant reconnects by itself. Entity IDs and
  history are unchanged, because they're derived from each device's hardware
  address.

Disabled scripts don't count, so you can switch back and forth without deleting
anything. The Automations page always shows which mode is active.

The connection is encrypted (ESPHome's Noise protocol, no plaintext fallback). If
Home Assistant runs somewhere else on your tailnet, add the hub manually as
`zigbee-hub.<tailnet>.ts.net`, port 6053; LAN discovery doesn't cross Tailscale.

### Network

The Zigbee network's channel, PAN ID, and the hub's direct links: every paired
device with its link quality and signal strength to the hub. Download a network backup
(keys, PAN, device table) and restore it onto a replacement board.

### Tailscale

Connection state, tailnet IP, MagicDNS name, and peers with whether each path is
direct or relayed through DERP. Change the auth key, hostname, or control server
(Headscale works too).

#### Tailnet only, no password

Once Tailscale is connected, you can switch the hub to **Tailnet only**. The UI,
the API, firmware updates and the Home Assistant connection then accept
connections only through Tailscale, and stop asking for a password: being on your
tailnet is the login.

- From the LAN the hub stops answering on port 80 and stops announcing itself.
  Packets forged to look like Tailscale traffic are dropped too, not just ignored
  by the web server.
- You can only turn it on from a browser that is itself connected over Tailscale,
  so you can't lock yourself out by switching it on from a connection that's
  about to be cut off.
- If your tailnet is shared with other people, restrict who can reach
  `zigbee-hub` with a [Tailscale ACL](https://tailscale.com/kb/1018/acls).

**Locked out?** (expired key, tailnet changed.) Hold the board's BOOT button for
10 seconds: LAN access with the password re-opens for 15 minutes. Or over USB,
run `./flash.sh --prov-only --lan-access`. Zigbee and your automations keep running
throughout.

### Status

Everything here is read-only: Wi-Fi (network, channel, signal, IP, time to come
online after boot), Wi-Fi recovery counters, memory (internal and PSRAM, current
and lowest since boot), uptime, reset reason, and the running firmware (version,
build, slot, whether it's still on trial, the last update's result).

### Settings

Everything you can change: the primary and backup Wi-Fi networks, firmware
updates, restarting the hub, and the diagnostic log download.

**Diagnostic log.** The hub keeps its own log on flash: warnings, errors, Wi-Fi
events, updates and reboots (with the reason), plus a status sample every 5
minutes, all with UTC timestamps. **Download log** in Settings gets the
last ~500 KB, so you can look into a drop or a crash from last night without
ever connecting a cable. It survives reboots, and a planned or watchdog reboot
writes everything out before restarting.

## Updating the firmware remotely

No USB cable needed after the first flash, and it works from anywhere on your
tailnet.

```bash
./flash.sh --ota                    # build and send to zigbee-hub over the network
./flash.sh --ota --host 100.x.y.z   # or a specific address
```

Or upload a `.bin` under **Settings → Firmware update** in the UI.

What happens:

1. The board writes the new firmware into its spare slot while the current one
   keeps running. Zigbee and your automations aren't interrupted during the upload.
2. It reboots into the new firmware right away: an update is something you
   started on purpose.
3. The new firmware is **on trial** until Zigbee is up and your automations are
   running. If it crashes or hangs before that, the board automatically goes back
   to the previous version, and the UI and `flash.sh` tell you it rolled back.

Your Zigbee network, devices, scripts and credentials are untouched by updates.

**Signed updates (optional).** Run `./flash.sh --gen-signing-key` once and every
build is signed with a key that stays on your machine. From then on, the board
accepts only updates signed with that key, even from someone who has the admin
password. Back the key up: without it, future updates need the USB cable again.
USB flashing always works either way.

## How it works

```
 Web UI (REST + WebSocket, port 80, LAN + tailnet)
 ─────────────────────────────────────────────────────────────
 Event bus
 ─────────────────────────────────────────────────────────────
 Zigbee        │ Device registry │ Automation     │ Tailscale
 coordinator   │ + persistence   │ engine (Lua)   │ WireGuard
 (2.4 GHz)     │ (LittleFS)      │  ── or ──      │ in lwIP
               │                 │ ESPHome API    │
               │                 │ (HA, optional) │
 ─────────────────────────────────────────────────────────────
 Wi-Fi station, 5 GHz only · radio coexistence · provisioning
```

- **Zigbee**: Espressif's Zigbee stack (`esp-zigbee-lib`) running as coordinator
  on the C5's native 802.15.4 radio. On first boot it forms a network on the
  quietest of channels 15 and 25, with a random PAN ID and a network key from the
  hardware RNG.
- **Wi-Fi**: station mode, band locked to 5 GHz before the radio starts, with no
  fallback. WPA2 minimum, WPA3 when the router offers it. If the access point
  disappears, a watchdog reconnects, then restarts the Wi-Fi driver, and after 20
  minutes of downtime reboots the board (again at 30 and 60 minutes if it's still
  down), but only at a safe moment (no timer due soon, nothing pairing or updating). The
  System page shows how many times it has done so.
- **Tailscale**: a native client (Tailscale's control protocol, WireGuard, DERP,
  DISCO, STUN) based on [microlink](https://github.com/CamM2325/microlink). The
  WireGuard interface sits in the network stack next to Wi-Fi, so the same web
  server answers on both.
- **Automations**: Lua 5.5. All scripts run on one dedicated task, fed events
  through a queue; Lua never runs on the Zigbee or web server task.
- **Home Assistant**: ESPHome native API on TCP 6053 with Noise encryption, Zigbee
  devices as ESPHome sub-devices (needs Home Assistant 2025.7 or newer). It uses
  no memory unless the integration is on and active.
- **One command path**: a command from a Lua script or from Home Assistant goes
  through the same code to the Zigbee stack.

### Flash layout (16 MB)

| Partition | Size | Holds |
|---|---|---|
| `nvs` | 24 KB | settings |
| `prov` | 16 KB | Wi-Fi + Tailscale credentials, admin password, optional Home Assistant key (written by `flash.sh`) |
| `ota_0` / `ota_1` | 4 MB each | firmware, A/B updates |
| `zb_storage` | 1 MB | Zigbee network state |
| `storage` | ~7 MB | device registry, scripts |

## Development

```bash
source ./env.sh            # activates the in-folder toolchain
idf.py build
idf.py -p "$ESPPORT" flash monitor
```

`env.sh` points ESP-IDF, its tools, Python (uv-managed) and every cache at `.esp/`.
Delete `.esp/` and the toolchain is gone without a trace.

| | |
|---|---|
| ESP-IDF | v6.1 |
| Compiler | riscv32-esp-elf-gcc 15.2 |
| Python | 3.14, uv-managed, used only by ESP-IDF's build scripts |

## Troubleshooting

**Board doesn't join Wi-Fi.** Check that the network has a 5 GHz radio. The board
never falls back to 2.4 GHz. DFS channels (52–144) work: the hub was developed on
one and follows the router when it changes channel. But the C5 can't detect radar
itself, so if you see frequent drops, set the router's 5 GHz radio to 36, 40, 44
or 48.

**A device won't pair.** Factory-reset it (usually a long button press or a power
cycle sequence), start a scan, and keep it close to the hub for the interview.
Sleepy battery devices may need a button press to wake up during the interview.

**Script stopped.** Its last error and line number are on its card in
Automations. Fix and save; it restarts.

**Home Assistant shows the devices as unavailable.** A script is enabled, so the
hub is the brain. Disable all scripts on the Automations page and Home Assistant
reconnects. If it's still unavailable, check that the integration is on under
Settings.

**UI not reachable over Tailscale.** The Tailscale page shows the connection
state. The auth key must be valid and the device approved in the admin console if
your tailnet requires device approval.

## Credits

- [microlink](https://github.com/CamM2325/microlink) (MIT, Cameron Malone): the
  Tailscale client this is built on
- [wireguard-lwip](https://github.com/smartalock/wireguard-lwip) (BSD-3): the
  WireGuard interface
- [Lua](https://www.lua.org/) (MIT, PUC-Rio)
- [zigbee2esphome](https://github.com/moag1000/zigbee2esphome): hard-won,
  measured lessons about Zigbee and Wi-Fi on the ESP32-C5
- [ESPHome](https://github.com/esphome/esphome): the native API's wire format
  and entity model
