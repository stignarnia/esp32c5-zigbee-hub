-- Door alarm: when a door opens, sound both buzzers and call an Alexa routine.
-- Automations -> GPIO: "buzzer1" and "buzzer2" as outputs (and a "ground" pin if a
-- buzzer's second leg sits on a GPIO). Automations -> Secrets: "alexa_alarm_url".
local DOORS = { "Front door", "Back door" }   -- names from the Devices page
gpio.claim("buzzer1", "buzzer2")   -- pins used later, in the handler: claimed now, at Save

-- 2 s per round: buzzers alternating, both stuttering, both on (they beat), pause
local ALARM = {}
for i = 1, 10 do ALARM[#ALARM + 1] = {1, 40}; ALARM[#ALARM + 1] = {2, 40} end
for i = 1, 10 do ALARM[#ALARM + 1] = {3, 30}; ALARM[#ALARM + 1] = {0, 30} end
ALARM[#ALARM + 1] = {3, 500}; ALARM[#ALARM + 1] = {0, 100}

local function alarm(door)
  log(door .. " opened")
  gpio.pattern({"buzzer1", "buzzer2"}, ALARM, {rounds = 5})   -- starts now, never waits
  local url = secret("alexa_alarm_url")
  if url then
    -- queued; if the network is down it is sent once it's back (for up to 10 min)
    http.get(url, {retry_for = 600, on_done = function(status, body)
      if status > 0 then log("Alexa: HTTP " .. status) else log("Alexa not reached: " .. body) end
    end})
  end
end

for _, door in ipairs(DOORS) do
  zb.on(door, "open", function(open) if open then alarm(door) end end)
end
