print("loaded lifecycle.lua")

Delay(0.5, function()
    print("delayed callback")
end)

events.presentQueue:Add(function()
    print("present")
end)

events.frameStageNotify:Add(function(stage)
    print("frame stage", stage)
end)

events.input:Add(function(first, second, third)
    print("input", first, second, third)
end)

events.menuToggled:Add(function(open)
    print("menu toggled", open)
end)

function __shutdown()
    print("shutdown lifecycle.lua")
end
