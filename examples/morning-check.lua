-- Weekday mornings at 7:00: log which doors are still open.
-- cron needs the clock (from the network) and the time zone (Settings -> Time zone).
cron("0 7 * * 1-5", function()
  local open = {}
  for _, d in ipairs(zb.devices()) do
    if d.kind == "contact" and zb.get(d.name, "open") then open[#open + 1] = d.name end
  end
  if #open > 0 then log("still open: " .. table.concat(open, ", ")) else log("all doors closed") end
end)
