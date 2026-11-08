-- zb.on(device, attr, function(value, device, attr) end)   attr: "open", "motion",
--   "water", "smoke", "alarm", "tamper", "battery", "temperature", "humidity",
--   "occupied", "available"; device "*" / attr "*" = any
-- zb.get(device, attr)  zb.devices()
-- timer.after(s, fn) / timer.every(s, fn) -> id; timer.cancel(id)
--   (a global function's name instead of fn survives a restart)
-- cron("min hour day month weekday", fn)
-- state.get(key) / state.put(key, value)   kept on flash
-- secret(name)   http.get(url, opts) / http.post(url, body, opts) / http.request{...}
--   opts: headers, timeout, retry_for, on_done = function(status, body) end
-- gpio.write/read/on/beep/pattern/stop/tone   (pins from Automations -> GPIO)
--   gpio.claim(pin, ...) at the top for pins only used in handlers: a pin belongs
--   to one script, and claims happen only while the script loads (so at Save)
-- led.set(r, g, b) / led.off()   the board's RGB LED (0..255); led.claim() at the top
--   if only handlers use it. One script owns it; it goes off when that script stops
-- net.online()  net.on("up"|"down", fn)   json.encode/decode   log(...)

log("hello from", "the hub")
