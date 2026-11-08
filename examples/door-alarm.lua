-- Door alarm: while a door is open, sound all three buzzers; call an Alexa routine once when
-- the alarm starts. Closing every door stops the buzzers.
-- Automations -> GPIO: "buzzer1", "buzzer2", "buzzer3" as outputs (and a "ground" pin if a
-- buzzer's second leg sits on a GPIO). Automations -> Secrets: "alexa_alarm_url".
local DOORS = { "Door", "Keys" }   -- names from the Devices page
gpio.claim("buzzer1", "buzzer2", "buzzer3")   -- pins used later, in the handlers: claimed now, at Save

-- 2 s per round: chase 1->2->3, all three stuttering, rotating pairs, all on (they beat), pause
local A, B, C = 1, 2, 4
local ALARM = {}
for i = 1, 6 do ALARM[#ALARM + 1] = {A, 30}; ALARM[#ALARM + 1] = {B, 30}; ALARM[#ALARM + 1] = {C, 30} end
for i = 1, 8 do ALARM[#ALARM + 1] = {A + B + C, 25}; ALARM[#ALARM + 1] = {0, 25} end
for i = 1, 4 do ALARM[#ALARM + 1] = {A + B, 50}; ALARM[#ALARM + 1] = {B + C, 50}; ALARM[#ALARM + 1] = {C + A, 50} end
ALARM[#ALARM + 1] = {A + B + C, 400}; ALARM[#ALARM + 1] = {0, 60}
local MAX_ROUNDS = 300   -- 10 minutes, the longest a pattern may play

local open = {}          -- door name -> true while it is open
local function any_open()
  for _, v in pairs(open) do if v then return true end end
  return false
end

local function alarm_start(door)
  log(door .. " opened: alarm on")
  gpio.pattern({"buzzer1", "buzzer2", "buzzer3"}, ALARM, {rounds = MAX_ROUNDS})   -- starts now, never waits
  local url = secret("alexa_alarm_url")
  if url then
    -- queued; if the network is down it is sent once it's back (for up to 10 min)
    http.get(url, {retry_for = 600, on_done = function(status, body)
      if status > 0 then log("Alexa: HTTP " .. status) else log("Alexa not reached: " .. body) end
    end})
  end
end

for _, door in ipairs(DOORS) do
  zb.on(door, "open", function(is_open)
    local was_on = any_open()
    open[door] = is_open
    if is_open and not was_on then
      alarm_start(door)
    elseif is_open then
      log(door .. " opened too")
    elseif was_on and not any_open() then
      gpio.stop()
      log(door .. " closed: alarm off")
    end
  end)
end
