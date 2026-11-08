-- Keys alarm: never let anyone walk out and get locked out without a warning.
-- Setup:
--   - "Door": contact sensor on the front door.
--   - "Keys": contact sensor on the inside of the door with no fixed magnet; a magnet is glued
--     to the key. Touching the key to "Keys" means "I'm leaving with it".
--   - Settings -> Motion: the hub senses movement through Wi-Fi. Place it so the walk to the
--     door crosses the line between the hub and the router.
--   - Automations -> GPIO: "buzzer1", "buzzer2", "buzzer3" as outputs (plus ground pins).
--   - Automations -> Secrets: "alexa_alarm_url".
-- Rules (nothing is remembered from one opening to the next, so closing the door is the reset):
--   - The Door opens within 60 s of a Keys tap: silent (leaving with the key). One tap, one opening.
--   - The Door opens and nothing moved inside in the minute before: silent (someone coming home).
--   - Anything else: alarm, 3 buzzers + the Alexa routine, 10 s. A Keys tap or closing the Door
--     stops it; the tap changes nothing else. Alexa is called at once, but not again within 30 s.
--   - No motion signal (Wi-Fi down, hub just started): counts as movement, so it sounds.

local DOOR = "Door"
local KEYS = "Keys"
local KEYS_WINDOW = 60  -- seconds a tap covers the next opening
local STILL_S = 60      -- no movement for this long before the opening = opened from outside
local GAP_S = 2         -- the last seconds before the opening are the opening itself
local ALEXA_COOLDOWN = 30  -- seconds before Alexa is called again

gpio.claim("buzzer1", "buzzer2", "buzzer3")

-- 2 s per round: chase 1->2->3, all three stuttering, rotating pairs, all on (they beat), pause
local A, B, C = 1, 2, 4
local ALARM = {}
for i = 1, 6 do ALARM[#ALARM + 1] = {A, 30}; ALARM[#ALARM + 1] = {B, 30}; ALARM[#ALARM + 1] = {C, 30} end
for i = 1, 8 do ALARM[#ALARM + 1] = {A + B + C, 25}; ALARM[#ALARM + 1] = {0, 25} end
for i = 1, 4 do ALARM[#ALARM + 1] = {A + B, 50}; ALARM[#ALARM + 1] = {B + C, 50}; ALARM[#ALARM + 1] = {C + A, 50} end
ALARM[#ALARM + 1] = {A + B + C, 400}; ALARM[#ALARM + 1] = {0, 60}
local ROUNDS = 5  -- 10 s

local tap_tmr = nil   -- set while a tap covers the next opening
local ring_tmr = nil  -- set while the alarm sounds
local alexa_cool = false

local function stop_alarm(why)
  if not ring_tmr then return end
  timer.cancel(ring_tmr)
  ring_tmr = nil
  gpio.stop()
  log(why .. ": alarm off")
end

local function start_alarm(why)
  log("Door opened: alarm (" .. why .. ")")
  if ring_tmr then timer.cancel(ring_tmr) end
  ring_tmr = timer.after(ROUNDS * 2, function() ring_tmr = nil end)
  gpio.pattern({"buzzer1", "buzzer2", "buzzer3"}, ALARM, {rounds = ROUNDS})
  local url = secret("alexa_alarm_url")
  if url and not alexa_cool then
    alexa_cool = true
    timer.after(ALEXA_COOLDOWN, function() alexa_cool = false end)
    http.get(url, {retry_for = 600, on_done = function(status, body)
      if status > 0 then log("Alexa: HTTP " .. status) else log("Alexa not reached: " .. body) end
    end})
  end
end

zb.on(KEYS, "open", function(is_open)
  if is_open then return end  -- only open -> close (key brought to the sensor) is a tap
  if ring_tmr then stop_alarm("Keys tapped"); return end
  if zb.get(DOOR, "open") == true then return end  -- door already open: nothing to cover
  if tap_tmr then timer.cancel(tap_tmr) end
  tap_tmr = timer.after(KEYS_WINDOW, function() tap_tmr = nil end)
  log("Keys tapped: the next opening within " .. KEYS_WINDOW .. " s is silent")
end)

zb.on(DOOR, "open", function(is_open)
  if not is_open then stop_alarm("Door closed"); return end
  if tap_tmr then
    timer.cancel(tap_tmr)
    tap_tmr = nil
    log("Door opened after a Keys tap: silent")
    return
  end
  local moved = motion.moved(STILL_S + GAP_S, GAP_S)
  if moved == false then
    log("Door opened, nothing moved inside for " .. STILL_S .. " s: coming home, silent")
    return
  end
  start_alarm(moved == nil and "no motion signal" or "movement inside before the opening")
end)

log("keys-alarm ready")
