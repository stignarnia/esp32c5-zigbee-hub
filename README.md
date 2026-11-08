# ESP32-C5 Local Zigbee Hub

> **Work in progress.** This README describes the finished product. Working today:
> 5 GHz-only Wi-Fi with backup network and recovery, provisioning via `flash.sh`,
> the web UI, over-the-air updates with rollback, the Zigbee coordinator with
> enrollment of battery sensors (tested with contact sensors), Tailscale (direct
> paths, relays, the "tailnet only, no password" mode), and Lua automations with
> buzzers and HTTP calls. Home Assistant, network backup and signed updates are
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
  integration: every Zigbee device appears in Home Assistant as a native ESPHome
  device, the hub adds its own entities (including its Wi-Fi motion sensor), and your
  automations can publish their own on top. No MQTT, no ZHA, no zigbee2mqtt.

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

**ESP32-C5 with 16 MB flash and 8 MB PSRAM** (developed on a Waveshare
ESP32-C5-WIFI6-KIT with the N16R8 module; any C5 board with PSRAM and a USB-UART
works). No wiring needed, except for optional buzzers or other GPIO devices your
scripts drive.

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
./flash.sh
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
✓ Connected to MyWiFi on 5 GHz, channel 44, -47 dBm, 1.7 s after boot
✓ Zigbee network up on channel 25 (PAN 0x7A3C)
✓ Tailscale: zigbee-hub.<tailnet>.ts.net (100.101.12.7)

  Web UI:   http://zigbee-hub.local/   (LAN, 192.168.1.50)
            http://zigbee-hub/         (tailnet)
  Password: 7f3k-qp9x-m2va
```

To put the hub on your tailnet, paste an auth key in the UI under **Settings →
Tailscale** (the Tailscale line appears from the next boot on). Create it in the
[Tailscale admin console](https://login.tailscale.com/admin/settings/keys): not
reusable, not ephemeral, pre-approved. The hub uses it once to register and then
erases it.

### Other flash options

| | |
|---|---|
| `./flash.sh --prov-only` | re-write the Wi-Fi credentials without rebuilding. The Zigbee network, your devices and the Tailscale login are untouched |
| `./flash.sh --prov-only --lan-access` | locked out of *tailnet only* access: turns it off, so the LAN with the password works again |
| `./flash.sh --hostname kitchen-hub` | different hostname on the LAN and the tailnet |
| `./flash.sh --ssid X --psk Y` | provision a network other than the one you're on |
| `./flash.sh --monitor` | stay attached to the serial log after flashing |
| `./flash.sh --ha` | turn on the Home Assistant integration and print its encryption key |
| `./flash.sh --ota` | update over the network instead of USB (see below) |
| `./flash.sh --gen-signing-key` | create a key so the board only accepts updates you signed |

Credentials live in their own flash partition. Re-provisioning never wipes the
Zigbee network, so you never have to re-pair devices because the Wi-Fi password
changed. `flash.sh` remembers your backup network and hostname between runs.

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

Log in with the admin password (or not at all, in *tailnet only* mode). The UI
updates live over a WebSocket and works on a phone. Four tabs: **Devices**,
**Automations**, **Status** (read-only) and **Settings** (everything that changes
something).

### Devices

Every paired sensor with its live state: contact, motion, water leak, smoke,
temperature, humidity and battery. Each device shows signal strength, last seen
and availability. Rename, re-interview or remove them. The UI only shows state;
there are no switches in it, and the hub never sends commands to devices.

The last known state of each sensor is kept across restarts, so a door that was
open before a reboot still reads open after it.

Below the list: the Zigbee network (channel, PAN ID) and a network map of the hub's
direct links, with each device's link quality and signal strength.

#### Adding a device

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
the Devices page and in your scripts (which refer to devices by that name).

### Automations

Write or paste Lua, press **Save**. The script is syntax-checked before it's
stored, then starts immediately and again after every boot, before Wi-Fi is up.
Scripts refer to devices by the names you gave them on the Devices page.

```lua
-- Front door left open for 5 minutes: beep and send a notification
gpio.claim("buzzer")          -- pins used later, in handlers, are claimed up front
local door_timer

zb.on("Front door", "open", function(open)
  if open then
    door_timer = timer.after(300, function()
      gpio.beep("buzzer", {on = 200, off = 200, times = 5})
      http.post(secret("ntfy_url"), "The front door has been open for 5 minutes")
    end)
  elseif door_timer then
    timer.cancel(door_timer)
    door_timer = nil
  end
end)
```

```lua
-- Leak sensor: sound the buzzer at once, then notify (retried until Wi-Fi is back)
gpio.claim("buzzer")
zb.on("Laundry leak", "water", function(wet)
  if wet then
    gpio.beep("buzzer", {on = 500, off = 500, times = 20})
    http.post(secret("ntfy_url"), "Water leak in the laundry room!", {retry_for = 3600})
  end
end)
```

```lua
-- Weekday mornings at 7: report sensors with a low battery
cron("0 7 * * 1-5", function()
  for _, d in ipairs(zb.devices()) do
    local b = zb.get(d.name, "battery")
    if b and b < 20 then http.post(secret("ntfy_url"), d.name .. " battery at " .. b .. "%") end
  end
end)
```

The Automations page has everything scripts need, top to bottom:

- **Scripts**: each with its state (running, off, stopped by an error and why),
  memory, how often it ran, its timers and cron jobs; switch on/off, edit, restart,
  delete. With the Home Assistant integration on, it also shows which mode is active.
- **Pins**: every enabled GPIO, its level, and the script that owns it.
- **Editor**: syntax highlighting, search, bracket matching, and completion of the
  hub's own functions (CodeMirror). Built-in examples to start from (the files in
  [`examples/`](examples/), offered in the editor; none is installed until you save it). If a script
  fails to load (a syntax error, a pin another script owns), the error is shown
  right there and its line is selected.
- **Secrets** and **GPIO** settings (below).
- **Script log**: what scripts `log()`, plus starts, stops and errors, live.

#### Script API

| | |
|---|---|
| `zb.on(device, attribute, fn)` | call `fn(value, device, attribute)` when it changes. `"*"` as device or attribute matches any |
| `zb.get(device, attribute)` | current value, `nil` if not known yet |
| `zb.devices()` | list of `{name, ieee, model, manuf, kind, available}` |
| `timer.after(s, fn)` / `timer.every(s, fn)` | one-shot / repeating timer, returns an id. Pass a global function's *name* instead of `fn` and the timer survives a restart |
| `timer.cancel(id)` | stop a timer or cron job |
| `cron(expr, fn)` | standard 5-field cron (`"min hour day month weekday"`), local time; returns an id |
| `state.get(key)` / `state.put(key, value)` | small key-value store kept on flash (strings, numbers, booleans) |
| `http.request{url=, method=, headers=, body= or json=, timeout=, retry_for=, on_done=}` | HTTP(S) request; never blocks, the result arrives in `on_done(status, body)` |
| `http.get(url, opts)` / `http.post(url, body, opts)` | shorthands for the above |
| `secret(name)` | a value stored under **Automations → Secrets**, so it never appears in script text |
| `net.online()` / `net.on("up" or "down", fn)` | whether the hub has Wi-Fi with an address now / be told when that changes |
| `motion.moving()` / `motion.moved(from_s, to_s)` | Wi-Fi motion sensing (Settings → Motion): moving now / any movement between `from_s` and `to_s` seconds ago (up to 600). `nil` when the hub couldn't see (no Wi-Fi signal, just started) |
| `json.encode(v)` / `json.decode(s)` | |
| `log(...)` (or `print`) | to the script log |
| `gpio.claim(pin, ...)` | take pins for this script when it loads (needed for pins only used in handlers) |
| `gpio.write(pin, v)` / `gpio.read(pin)` | drive / read a pin |
| `gpio.on(pin, "rising" or "falling" or "both", fn)` | call `fn(level, pin)` on an edge (debounced) |
| `gpio.beep(pin, ms)` / `gpio.beep(pin, {on=, off=, times=})` | sound an **active** buzzer |
| `gpio.pattern(pins, steps, {rounds = n})` | play several buzzers in step: `steps = {{mask, ms}, …}`, bit *i* of `mask` turns `pins[i]` on |
| `gpio.tone(pin, hz, ms)` | a tone on a **passive** buzzer |
| `gpio.stop(pins)` | end a beep, pattern or tone |
| `led.set(r, g, b)` / `led.off()` | the board's RGB LED, 0–255 per colour |
| `led.claim()` | take the LED for this script when it loads (needed if only handlers use it) |

Device attributes: `open` (contact sensors), `motion`, `water`, `smoke`, `co`,
`vibration` (or `alarm` for any of these), `tamper`, `battery_low`, `battery` (%),
`battery_v`, `temperature` (°C), `humidity` (%), `occupied`, `available`.

Everything that waits (timers, cron, HTTP, buzzer patterns) happens in the
background: a script's code runs, returns, and is called again when something
happens. Patterns and beeps are timed by the hub, not by Lua, so 30 ms steps stay
exact.

#### Buzzers and other GPIO

Enable a pin under **Automations → GPIO**, give it a name, and scripts can use it:

```lua
-- Door left open for 2 minutes: beep three times, every minute, until it closes
gpio.claim("buzzer")
local nag
zb.on("Front door", "open", function(open)
  if not open then
    if nag then timer.cancel(nag); nag = nil end
  else
    nag = timer.after(120, function()
      nag = timer.every(60, function() gpio.beep("buzzer", {on = 150, off = 100, times = 3}) end)
    end)
  end
end)
```

Only pins you've enabled are usable: GPIO 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 23, 24,
25 and 26, plus GPIO 27 as a ground only. The rest are taken by the board itself and
can't be enabled at all. Outputs start low. A pin set to **ground** stays low, for a
buzzer whose second leg sits on a GPIO instead of GND; say which output it's the
**ground for**.

**The RGB LED** (GPIO 27) can be used together with a buzzer grounded on GPIO 27:
the hub sends a colour only while that buzzer is silent, and lets go of the buzzer's
output during the ~80 µs it takes, so no current flows through the buzzer. A colour
set while the buzzer plays is shown as soon as it stops. If a ground pin doesn't say
which output it's for, the hub waits for every output to be low. The LED turns off
when the script that owns it stops, and at every start of the hub (it would
otherwise keep its last colour through a restart).

Every GPIO of the Waveshare ESP32-C5-WIFI6-KIT-NXRX (N16R8), from its
[schematic](https://files.waveshare.com/wiki/ESP32-C5-WIFI6-KIT-NXRX/ESP32-C5-WIFI6-KIT-NXRX-Schematic.pdf)
and the [ESP32-C5 datasheet](https://documentation.espressif.com/esp32-c5_datasheet_en.pdf)
v1.5 (§2 Pins, §3 Boot Configurations). "At reset" is what the chip reads from the pin
at the moment of reset, on this chip's eFuses (all factory: `UART_PRINT_CONTROL` 0,
`JTAG_SEL_ENABLE` 0, `DIS_PAD_JTAG` 0, `DIS_USB_JTAG` 0, read from the board):

| GPIO | On the board | Chip function | At reset | Scripts |
|---|---|---|---|---|
| 0 | header; 32 kHz crystal Y1 **not fitted** | 32 kHz crystal pad | — | yes |
| 1 | header; crystal not fitted | 32 kHz crystal pad, ADC | — | yes |
| 2 | header | JTAG TMS, ADC | strapping, but no boot setting uses it | yes |
| 3 | header | JTAG TDI, ADC | SDIO-slave clock edge (with 25): never used | yes |
| 4 | header | JTAG TCK, ADC | — | yes |
| 5 | header | JTAG TDO, ADC | — | yes |
| 6 | header **and the battery-voltage divider** (200 k/100 k, 100 nF) | ADC | — | yes; with a battery fitted, driving it loses the battery reading |
| 7 | header, no pull resistor | — | JTAG source: ignored (`JTAG_SEL_ENABLE` 0) | yes |
| 8, 9, 10 | header | — | — | yes |
| 11 | header "TX" → USB-serial chip (CH343, "UART" port) | UART0 TX: logs, flashing | — | **no** |
| 12 | header "RX" ← driven by the CH343 | UART0 RX | — | **no** |
| 13, 14 | header → "USB" USB-C port (D−, D+) | native USB | — | **no** |
| 15 | header "NC/GPIO15" | PSRAM chip select | — | **no**: driving it crashes the chip |
| 16–22 | inside the module | flash/PSRAM bus, flash supply (19) | — | **no** |
| 23, 24 | header | — | — | yes |
| 25 | header | — | SDIO-slave clock edge (with 3): never used | yes |
| 26 | header | — | boot mode, only while BOOT is held | yes |
| 27 | header, 10 k pull-up, **RGB LED** (WS2812B) data | — | ROM log printing (ignored, eFuse 0); boot mode with BOOT held (pull-up keeps it high) | ground only |
| 28 | header, 10 k pull-up, **BOOT button**, auto-download circuit | — | **run or wait to be flashed** | **no** |

GPIO 0 and 1 are free only because this board leaves the 32 kHz crystal out. On a
board that fits one, leave them alone.

Each pin belongs to one script at a time, and a script takes its pins when it
loads: any `gpio` call at the top of the script claims that pin, and
`gpio.claim(...)` claims pins it only uses later, in handlers. A handler can't take
a new pin. So if two scripts want the same buzzer, you find out when you press
**Save**, not when the door opens. If a script stops for any reason (disabled,
error, out of memory), its pins go low and any beep stops, so a crashed script
never leaves a buzzer sounding.

**Wiring:** outputs run at full drive strength. The common 12 mm "3 V active"
buzzers (they're magnetic, ~20 mA average, short peaks around 50 mA) worked
connected straight to a pin and GND, measured at 3.0 V under load. **Solder them
or seat them firmly**: a buzzer that makes and breaks contact throws voltage
spikes big enough to reset the chip (seen with one sitting loose in the header).
One beep pulse is limited to 2 s. Anything bigger than such a buzzer needs a small
NPN transistor or logic-level MOSFET, and a flyback diode across anything
magnetic.

#### Calling other services: Alexa, webhooks

Scripts can make HTTP and HTTPS requests. Certificates are verified against the
usual public authorities; plain `http://` only goes to addresses on your LAN or
tailnet. The request goes out in the background, so a slow or offline service
never delays the rest of the script; the local buzzer still sounds at once.

Alexa has no webhook of its own, but third-party skills (routine triggers,
announcement services) give you an HTTPS URL that runs an Alexa routine when
called, for example one that plays an alarm on every Echo in the house. Store that
URL under **Automations → Secrets** as `alexa_alarm`, then:

```lua
-- Water leak: buzzer here, alarm on the Echos, and keep trying for 10 minutes
-- if the internet happens to be down
gpio.claim("buzzer")
zb.on("Laundry leak", "water", function(wet)
  if not wet then return end
  gpio.beep("buzzer", {on = 500, off = 300, times = 20})
  http.get(secret("alexa_alarm"), {
    retry_for = 600,
    on_done = function(status, body)
      if status ~= 200 then log("Alexa alarm failed: " .. status .. " " .. body) end
    end,
  })
end)
```

`retry_for` is what makes it dependable: without it a request made while offline
is dropped; with it, the hub keeps retrying (offline, timeouts, 5xx and 429) with
backoff until the deadline. Status `0` means the request never got an answer; then
`body` says why. A `200` means the service accepted the call, not that an Echo
actually played: no public API tells you that.

Secrets are write-only: the UI shows their names, never their values, and if a
script logs one, the log shows `***` instead.

#### Safety and limits

Each script runs in its own sandbox: no files, no loading code from strings or
bytecode, no `os.execute`. A script that errors, runs for more than 1 s without
returning (an endless loop), runs out of memory or nests calls too deeply is
stopped and its error is shown with the line number; the other scripts keep
running. If a script ever manages to crash the hub itself, the hub restarts with
that script switched off and says why, instead of crashing again on every boot.

The same goes for buzzers: if the hub resets unexpectedly (a crash, a watchdog, a
power glitch, a brownout) while a script's buzzer is playing, that script is switched
off at the next start, with the reason shown. A buzzer with a bad contact can throw a
voltage spike that resets the chip; this keeps it from doing so again on every boot.

**Safe mode**: if the hub resets unexpectedly three times in a row, each time less
than a minute after starting, it starts the next time without running any script
(nothing is switched off or deleted). Zigbee, Wi-Fi, the web UI and Tailscale run as
usual, and the Automations page shows a banner with **Start scripts**. Restarts you
cause (power on, the RST button, Settings → Restart, a firmware update) never count.

| Limit | |
|---|---|
| scripts | 16, up to 32 KB of source each |
| memory | 256 KB per script |
| run time | 1 s per call into a script |
| timers | 16 per script, 64 on the hub; cron jobs 32 |
| `state` | 64 keys per script, strings up to 1 KB |
| HTTP | 10 requests per minute per script, 16 queued, one at a time; responses cut at 16 KB, request bodies up to 8 KB |
| secrets | 16, up to 512 characters each |

**Without Wi-Fi**: device triggers, timers and buzzers always work. Cron needs the
time of day: it comes from the internet and the chip's clock keeps it through
outages and restarts. Only after a power cut with no internet does the hub not
know the time; then cron jobs wait (the Automations page says "clock not set")
instead of firing at the wrong moment.

**Time zone**: cron and `os.date` use local time, set under **Settings → Time
zone** (presets for common zones). It's a POSIX rule, which works without a time
zone database: `CET-1CEST,M3.5.0,M10.5.0/3` reads "standard time *CET*, 1 hour
ahead of UTC (the sign is inverted), summer time *CEST* (one more hour) from the
last (5) Sunday (0) of March (3) until the last Sunday of October at 03:00".

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

**Your automations add to this, they never replace it.** Every Zigbee device is always
there. Each script gets an on/off switch, and a script can publish its own entities
("alarm armed", "doors open: 2", "alarm triggered"). A disabled script's entities show
as unavailable and keep their history; deleting the script removes them. When the set
changes, Home Assistant reconnects by itself and picks it up. Entity IDs and history
never change, because they're derived from each device's hardware address (or the
script's name).

The connection is encrypted (ESPHome's Noise protocol, no plaintext fallback). If
Home Assistant runs somewhere else on your tailnet, add the hub manually as
`zigbee-hub.<tailnet>.ts.net`, port 6053; LAN discovery doesn't cross Tailscale.

### Tailscale

On the Status page: connection state, tailnet IP, MagicDNS name, the home relay
(DERP region), and peers with whether each path is direct or relayed. Under
Settings: paste or replace the auth key, turn Tailscale off, or log out of the
tailnet. The hub's name on the tailnet is its hostname (`./flash.sh --hostname`).

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
online after boot), Wi-Fi recovery counters with the last disconnect reason in
words, Tailscale, memory (internal and PSRAM, current and lowest since boot),
uptime, reset reason, and the running firmware (version, build, slot, whether it's
still on trial, the last update's result).

### Settings

Everything you can change that isn't about scripts: the primary and backup Wi-Fi
networks, Tailscale (auth key, on/off, log out, tailnet-only access), the Home
Assistant integration, firmware updates, the Zigbee network backup (download, and
restore onto a replacement board), the time zone, restarting the hub, and the
diagnostic log download. Script secrets and GPIO pins are on the Automations page.

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
 (2.4 GHz)     │ (LittleFS)      │  ── and ──     │ in lwIP
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
  down), but only at a safe moment (no timer or cron job due within 2 minutes, no
  script running, no buzzer playing, nothing pairing or updating), with script
  state and named timers saved first. The Status page shows how many times it has
  done so.
- **Tailscale**: a native client (Tailscale's control protocol, WireGuard, DERP,
  DISCO, STUN) based on [microlink](https://github.com/CamM2325/microlink). The
  WireGuard interface sits in the network stack next to Wi-Fi, so the same web
  server answers on both. The hub picks its home relay (DERP region) by measured
  latency, reaches peers homed elsewhere through their own region, and asks your
  router for a port mapping (NAT-PMP) so peers behind the same ISP can connect
  directly. It only probes paths when they're in use or the UI is open, so an idle
  hub stays quiet on the network.
- **Automations**: Lua 5.5, unmodified. Each script has its own Lua state with a
  capped heap in PSRAM; all of them run on one task (its 160 KB stack in PSRAM), fed
  events through a queue. Lua never runs on the Zigbee or web server task, and
  Zigbee never waits for a script: if the queue is ever full, events are dropped
  and counted rather than blocking. HTTP requests run on their own task, one at a
  time.
- **Home Assistant**: ESPHome native API on TCP 6053 with Noise encryption, Zigbee
  devices as ESPHome sub-devices (needs Home Assistant 2025.7 or newer). It uses
  no memory unless the integration is on and active.
- **Commanding devices**: scripts and Home Assistant can set devices that take
  commands (switches, lights); devices that only report (sensors) are read.

### Flash layout (16 MB)

| Partition | Size | Holds |
|---|---|---|
| `nvs` | 16 KB | settings, Wi-Fi driver state, Tailscale node keys, script secrets |
| `otadata` | 8 KB | which firmware slot boots |
| `prov` | 12 KB | Wi-Fi credentials, admin password, optional Home Assistant key (written by `flash.sh`) |
| `zb_storage` | 120 KB | Zigbee network state (keys, PAN, children) |
| `coredump` | 64 KB | crash dump of the last panic |
| `ota_0` / `ota_1` | 4 MB each | firmware, A/B updates |
| `storage` | ~7.75 MB | device registry (with each sensor's last state), scripts and their `state`, GPIO config, diagnostic log |

## Development

```bash
source ./env.sh            # activates the in-folder toolchain
idf.py build
idf.py -p "$ESPPORT" flash monitor
```

`env.sh` points ESP-IDF, its tools, Python (uv-managed) and every cache at `.esp/`.
Delete `.esp/` and the toolchain is gone without a trace.

The web UI (`components/web/ui/`) is plain HTML, CSS and JavaScript with no build
step; CMake gzips it into the firmware. The editor's example scripts are the
`examples/*.lua` files: at build time `components/web/gen_app_js.py` puts them into
`app.js`, named after the file (`door-alarm.lua` → "Door alarm"). Add or edit a file
there and rebuild; `template.lua` is what a new script starts from. The one exception is the script editor,
`cm.js`: a prebuilt CodeMirror bundle, committed. To change or update it, edit
`tools/codemirror/` (pinned versions in `package.json`) and run
`tools/codemirror/build.sh`; it needs Node and keeps npm's files in `.esp/`.

| | |
|---|---|
| ESP-IDF | v6.1 |
| Compiler | riscv32-esp-elf-gcc 15.2 |
| Python | 3.14, uv-managed, used only by ESP-IDF's build scripts |
| Node | only to rebuild the editor bundle (`tools/codemirror/build.sh`) |

## Troubleshooting

**Board doesn't join Wi-Fi.** Check that the network has a 5 GHz radio. The board
never falls back to 2.4 GHz. DFS channels (52–144) work: the hub was developed on
one and follows the router when it changes channel. But the C5 can't detect radar
itself, so if you see frequent drops, set the router's 5 GHz radio to 36, 40, 44
or 48.

**A device won't pair.** Factory-reset it (usually a long button press or a power
cycle sequence), start a scan, and keep it close to the hub for the interview.
Sleepy battery devices may need a button press to wake up during the interview.

**Script stopped.** Its last error and line number are next to it on the
Automations page, and in the script log. Fix and save; it restarts.

**Script switched off by itself, "the hub crashed while this script was running".**
The script made the hub crash or hang, so the hub turned it off to avoid crashing
again on every boot. Fix it and switch it back on. Seeing this means the hub has a
bug worth reporting, with the script that triggers it.

**"is used by script …" when saving.** Another script owns that GPIO pin. Each pin
belongs to one script; move the buzzer to another pin, or the code into one script.

**"is not claimed by this script".** A handler used a pin the script didn't take
when it loaded. Add `gpio.claim("name")` at the top of the script.

**Cron jobs don't fire.** Check the time zone under Settings, and the clock line on
the Automations page: after a power cut, cron waits until the hub has the time from
the internet.

**Home Assistant shows one hub device instead of my sensors.** A script is
enabled, so the hub is the brain and Home Assistant sees the hub as one device.
Disable all scripts on the Automations page to get the individual devices back. If
nothing shows up at all, check that the integration is on under Settings.

**UI not reachable over Tailscale.** The Tailscale page shows the connection
state. The auth key must be valid and the device approved in the admin console if
your tailnet requires device approval.

**Pings to the hub vary between a few ms and ~60 ms.** That's the radio being
shared: the C5 has one 2.4/5 GHz radio front end for Wi-Fi and Zigbee, and gives
Zigbee about 56 ms of every 102 ms. A packet that arrives in that window waits
for it to end. Zigbee comes first by design; a web UI doesn't notice.

**A peer shows "relay (DERP)" instead of "direct".** Direct paths are only kept
up while there's traffic or the UI is open, so an idle peer showing relay is
normal; open the UI and it turns direct within seconds if the networks allow it.
If it never does, the two networks block each other's UDP (common behind
carrier-grade NAT). Traffic still flows, through Tailscale's relay. A public IPv4
address from your ISP, or NAT-PMP/UPnP on the other side's router, usually fixes
it.

## Credits

- [microlink](https://github.com/CamM2325/microlink) (MIT, Cameron Malone): the
  Tailscale client this is built on
- [wireguard-lwip](https://github.com/smartalock/wireguard-lwip) (BSD-3): the
  WireGuard interface
- [Lua](https://www.lua.org/) (MIT, PUC-Rio)
- [CodeMirror](https://codemirror.net/) (MIT, Marijn Haverbeke and others): the
  script editor
- [zigbee2esphome](https://github.com/moag1000/zigbee2esphome): hard-won,
  measured lessons about Zigbee and Wi-Fi on the ESP32-C5
- [ESPHome](https://github.com/esphome/esphome): the native API's wire format
  and entity model
