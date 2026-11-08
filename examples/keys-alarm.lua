-- Keys alarm: don't leave home without your keys.
-- Setup:
--   - "Door": contact sensor mounted normally on the front door.
--   - "Keys": contact sensor mounted on the inside of the door (no fixed magnet);
--     a magnet is glued to your keys. Tapping the keys to "Keys" gives open -> close.
--   - Automations -> GPIO: "buzzer1", "buzzer2", "buzzer3" as outputs (plus ground pins).
--   - Automations -> Secrets: "alexa_alarm_url".
-- Rules:
--   - Tapping "Keys" (open -> close) while the door is closed marks you as inside (away=false)
--     and opens a 60 s window to open the door silently.
--   - Opening "Door" within 60 s of tapping "Keys" is a silent exit (away becomes true).
--   - Opening "Door" when away=true (coming back home) is always silent (away becomes false).
--   - Opening "Door" when away=false without a "Keys" tap in the last 60 s sounds the alarm
--     (3 buzzers + Alexa routine URL) and stays away=false.
--   - While the alarm is ringing, tapping "Keys" stops the alarm and counts as leaving
--     (away becomes true); closing "Door" without tapping "Keys" stops the buzzers and
--     keeps away=false.

local DOOR = "Door"
local KEYS = "Keys"
local KEYS_WINDOW = 60      -- seconds after tapping Keys to open Door
local ALEXA_COOLDOWN = 30   -- minimum seconds between Alexa routine calls

gpio.claim("buzzer1", "buzzer2", "buzzer3")

-- 2 s per round: chase 1->2->3, all three stuttering, rotating pairs, all on (they beat), pause
local A, B, C = 1, 2, 4
local ALARM = {}
for i = 1, 6 do ALARM[#ALARM + 1] = {A, 30}; ALARM[#ALARM + 1] = {B, 30}; ALARM[#ALARM + 1] = {C, 30} end
for i = 1, 8 do ALARM[#ALARM + 1] = {A + B + C, 25}; ALARM[#ALARM + 1] = {0, 25} end
for i = 1, 4 do ALARM[#ALARM + 1] = {A + B, 50}; ALARM[#ALARM + 1] = {B + C, 50}; ALARM[#ALARM + 1] = {C + A, 50} end
ALARM[#ALARM + 1] = {A + B + C, 400}; ALARM[#ALARM + 1] = {0, 60}
local MAX_ROUNDS = 300   -- 10 minutes, the longest a pattern may play

local away = state.get("away") == true
local keys_ok = false
local keys_tmr = nil
local ringing = false
local ring_tmr = nil
local alexa_cool = false

local function set_away(v)
  away = v
  state.put("away", v)
end

local function clear_keys_ok()
  if keys_tmr then timer.cancel(keys_tmr); keys_tmr = nil end
  keys_ok = false
end

local function stop_alarm()
  if ring_tmr then timer.cancel(ring_tmr); ring_tmr = nil end
  ringing = false
  gpio.stop()
end

local function start_alarm()
  ringing = true
  if ring_tmr then timer.cancel(ring_tmr) end
  ring_tmr = timer.after(MAX_ROUNDS * 2, function()
    ring_tmr = nil
    ringing = false
  end)
  gpio.pattern({"buzzer1", "buzzer2", "buzzer3"}, ALARM, {rounds = MAX_ROUNDS})
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
  if is_open then return end   -- only open -> close (magnet brought to Keys) counts
  if ringing then
    stop_alarm()
    clear_keys_ok()
    set_away(true)
    log("Keys tapped while ringing: alarm off, leaving (away=true)")
    return
  end
  if zb.get(DOOR, "open") == true and away then
    -- Door is already open for an in-progress exit; ignore a second tap/bounce on the way out.
    return
  end
  set_away(false)
  if keys_tmr then timer.cancel(keys_tmr) end
  keys_ok = true
  keys_tmr = timer.after(KEYS_WINDOW, function()
    keys_tmr = nil
    keys_ok = false
    log("Keys 60 s window expired")
  end)
  log("Keys tapped: 60 s window open (away=false)")
end)

zb.on(DOOR, "open", function(is_open)
  if is_open then
    if keys_ok then
      clear_keys_ok()
      set_away(true)
      log("Door opened with keys: silent exit (away=true)")
    elseif away then
      set_away(false)
      log("Door opened: welcome home (away=false)")
    else
      log("Door opened without keys: alarm on (away=false)")
      start_alarm()
    end
  else
    if ringing then
      stop_alarm()
      log("Door closed: alarm off (away=false)")
    end
  end
end)

log("keys-alarm ready (away=" .. tostring(away) .. ")")
