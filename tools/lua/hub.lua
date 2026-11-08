---@meta
-- The hub's Lua API, for editors with the Lua language server (LuaLS): completion, argument
-- hints, no "undefined global" warnings. Never runs; the firmware provides these.
-- Reference: README.md, "Automations". .luarc.json at the repo root points LuaLS here.

---@alias hub_pin integer|string  GPIO number or the name given under Automations -> GPIO

zb = {}
---Call fn(value, device, attribute) when the attribute changes. "*" matches any device / attribute.
---@param device string name from the Devices page, IEEE hex, or "*"
---@param attribute string "open", "motion", "water", "battery", "temperature", ... or "*"
---@param fn fun(value: boolean|number, device: string, attribute: string)
function zb.on(device, attribute, fn) end
---Current value; nil if not known yet. Last known value is kept across restarts.
---@param device string
---@param attribute string
---@return boolean|number|nil
function zb.get(device, attribute) end
---@return {name: string, ieee: string, model: string, manuf: string, kind: string, available: boolean}[]
function zb.devices() end

timer = {}
---One-shot timer. Pass a global function's name instead of fn and it survives a restart.
---@param s number seconds
---@param fn function|string
---@return integer id
function timer.after(s, fn) end
---Repeating timer. Pass a global function's name instead of fn and it survives a restart.
---@param s number seconds
---@param fn function|string
---@return integer id
function timer.every(s, fn) end
---Stop a timer or cron job.
---@param id integer
function timer.cancel(id) end

---Standard 5-field cron ("min hour day month weekday"), local time.
---@param expr string
---@param fn function
---@return integer id
function cron(expr, fn) end

state = {}
---@param key string
---@return string|number|boolean|nil
function state.get(key) end
---Small key-value store kept on flash.
---@param key string
---@param value string|number|boolean|nil
function state.put(key, value) end

---@class hub_http_opts
---@field url? string
---@field method? "GET"|"POST"|"PUT"
---@field headers? table<string, string>
---@field body? string
---@field json? table
---@field timeout? number seconds, default 10
---@field retry_for? number seconds to keep retrying while offline or on errors, default 0
---@field on_done? fun(status: integer, body: string) status 0 = network error / gave up

http = {}
---HTTP(S) request. Never blocks; the result arrives in on_done.
---@param opts hub_http_opts
function http.request(opts) end
---@param url string
---@param opts? hub_http_opts
function http.get(url, opts) end
---@param url string
---@param body string
---@param opts? hub_http_opts
function http.post(url, body, opts) end

---A value stored under Automations -> Secrets.
---@param name string
---@return string|nil
function secret(name) end

net = {}
---Wi-Fi up with an address, now.
---@return boolean
function net.online() end
---@param event "up"|"down"
---@param fn function
function net.on(event, fn) end

motion = {}
---Wi-Fi motion sensing (Settings -> Motion): moving now; nil when not sensing.
---@return boolean|nil
function motion.moving() end
---Any movement between from_s and to_s seconds ago (0 <= to_s < from_s <= 600); nil when the
---hub couldn't see part of that window (no Wi-Fi signal, just started).
---@param from_s number
---@param to_s? number default 0
---@return boolean|nil
function motion.moved(from_s, to_s) end

json = {}
---@param v any
---@return string
function json.encode(v) end
---@param s string
---@return any
function json.decode(s) end

---To the script log (print does the same). Secret values are masked.
---@param ... any
function log(...) end

---@class hub_beep
---@field on integer ms
---@field off integer ms
---@field times integer

gpio = {}
---Take pins for this script when it loads (needed for pins only used in handlers).
---@param ... hub_pin
function gpio.claim(...) end
---@param pin hub_pin
---@param v 0|1|boolean
function gpio.write(pin, v) end
---@param pin hub_pin
---@return 0|1
function gpio.read(pin) end
---Call fn(level, pin) on an edge (debounced).
---@param pin hub_pin
---@param edge "rising"|"falling"|"both"
---@param fn fun(level: integer, pin: integer)
function gpio.on(pin, edge, fn) end
---Sound an active buzzer: one pulse of ms, or a pattern.
---@param pin hub_pin
---@param ms integer|hub_beep
function gpio.beep(pin, ms) end
---Play several buzzers in step: steps = {{mask, ms}, ...}, bit i of mask turns pins[i] on.
---@param pins hub_pin[]
---@param steps integer[][]
---@param opts? {rounds: integer}
function gpio.pattern(pins, steps, opts) end
---A tone on a passive buzzer.
---@param pin hub_pin
---@param hz integer
---@param ms integer
function gpio.tone(pin, hz, ms) end
---End a beep, pattern or tone (all of this script's when pins is omitted).
---@param pins? hub_pin|hub_pin[]
function gpio.stop(pins) end

led = {}
---The board's RGB LED, 0-255 per colour.
---@param r integer
---@param g integer
---@param b integer
function led.set(r, g, b) end
function led.off() end
---Take the LED for this script when it loads (needed if only handlers use it).
function led.claim() end
