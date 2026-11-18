-- Keys alarm: never let anyone walk out and get locked out without a warning.
-- Setup:
--   - "Door": contact sensor on the front door.
--   - "Keys": contact sensor on the inside of the door with no fixed magnet; a magnet is glued
--     to the key. Touching the key to "Keys" means "I'm leaving with it".
--   - Settings -> Motion: the hub senses movement through Wi-Fi. Place it so the walk to the
--     door crosses the line between the hub and the router.
--   - Automations -> GPIO: "buzzer1", "buzzer2", "buzzer3" as outputs (plus ground pins).
--   - Home Assistant (Settings -> Home Assistant): the script publishes "Allarme", on for 30 s
--     when an alarm starts. A Home Assistant automation on it has an Echo say the reminder.
-- Rules (nothing is remembered from one opening to the next, so closing the door is the reset):
--   - The Door opens within 60 s of a Keys tap: silent (leaving with the key). One tap, one opening.
--   - The Door opens and nothing moved inside in the minute before: silent (someone coming home).
--   - Anything else: alarm, 3 buzzers for 10 s and "Allarme" on in Home Assistant. A Keys tap or
--     closing the Door stops the buzzers; the tap changes nothing else. "Allarme" turns on at
--     once and stays on 30 s; an alarm within those 30 s doesn't turn it on again.
--   - No motion signal (Wi-Fi down, hub just started): counts as movement, so it sounds.

local DOOR = "Door"
local KEYS = "Keys"
local KEYS_WINDOW = 60  -- seconds a tap covers the next opening
local STILL_S = 60      -- no movement for this long before the opening = opened from outside
local GAP_S = 2         -- the last seconds before the opening are the opening itself
local ALERT_S = 30     -- seconds "Allarme" stays on in Home Assistant (no new reminder meanwhile)

gpio.claim("buzzer1", "buzzer2", "buzzer3")
-- Defined at load: a new entity makes Home Assistant reload the hub's entities, and a value
-- published in that moment wouldn't reach it.
ha.binary("Allarme", false, {device_class = "safety"})

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
local alert_tmr = nil -- set while "Allarme" is on

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
  if not alert_tmr then
    ha.binary("Allarme", true)
    log("Allarme on: Home Assistant has the Echo say the reminder")
    alert_tmr = timer.after(ALERT_S, function()
      alert_tmr = nil
      ha.binary("Allarme", false)
    end)
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
