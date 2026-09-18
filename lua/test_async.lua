#!/usr/bin/env lua

-- conn:call_async() compared with conn:call().
--
-- The synchronous invoke runs its own event loop until the answer arrives, so
-- everything else the process is doing waits. The asynchronous one hands the
-- request to the uloop the connection is already attached to and returns; the
-- callback runs later, from that loop.
--
-- Run it and watch the tick counter: it stays at zero across the blocking
-- call and keeps climbing across the deferred one.

local ubus = require "ubus"
local uloop = require "uloop"

uloop.init()

local conn = ubus.connect()
if not conn then
	error("Failed to connect to ubus")
end

local ticks = 0
local timer
timer = uloop.timer(function()
	ticks = ticks + 1
	timer:set(10)
end, 10)

-- something that takes a moment; 'file' is provided by rpcd
local slow = { command = "sleep", params = { "1" } }

ticks = 0
conn:call("file", "exec", slow)
print(string.format("blocking call: %d timer ticks while it ran", ticks))

ticks = 0
conn:call_async("file", "exec", slow, function(result, status)
	print(string.format("deferred call: %d timer ticks while it ran, status %d",
		ticks, status))

	-- and the deadline: 3 seconds for a call that takes 5
	conn:call_async("file", "exec", { command = "sleep", params = { "5" } },
		function(_, code)
			print(string.format("timed out as expected, status %d (UBUS_STATUS_TIMEOUT is 7)",
				code))
			uloop.cancel()
		end, 3)
end)

uloop.run()
