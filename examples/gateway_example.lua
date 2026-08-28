-- gateway_example.lua
--
-- Example slcgw.exe gateway script demonstrating the full script
-- contract: initialize()/finalize(), dropping, editing a frame in
-- place, forwarding to one destination, fanning out to several, and
-- sending different frame content to different destinations.
--
-- Try it against a channel named "can0" with:
--   slcgw.exe can0=gateway_example.lua

local frame_count = 0

-- Called once when this channel starts. Returning a non-zero value
-- (or raising an error) would disable this channel only.
function initialize()
    print("gateway_example.lua: initialize")
    frame_count = 0
    return 0
end

-- Called once per frame received on the channel this script is
-- attached to (e.g. "can0" in the example command above).
--
--   src_channel - the channel name the frame arrived on (string)
--   frame       - a table: id, ext, rtr, fd, brs, esi, dlc, len,
--                 data (1-indexed array of len bytes). Editing these
--                 fields in place edits the frame before forwarding.
function gateway(src_channel, frame)
    frame_count = frame_count + 1

    -- Drop anything with a diagnostic ID range (0x700-0x7FF), just as
    -- an example of a pure filter with no editing or forwarding.
    if frame.id >= 0x700 and frame.id <= 0x7FF then
        return false
    end

    -- Remap one specific ID and forward it, edited, to a single
    -- other channel.
    if frame.id == 0x123 then
        frame.id = 0x456
        return "can1"
    end

    -- Mask the low byte of ID 0x200's first data byte (e.g. to strip
    -- a counter/checksum before re-broadcasting) and fan it out,
    -- unchanged after that edit, to two destinations at once.
    if frame.id == 0x200 then
        if frame.len > 0 then
            frame.data[1] = 0x00
        end
        return { "can1", "can2" }
    end

    -- Send genuinely different content to different destinations:
    -- can1 gets the frame as received, can2 gets a modified copy
    -- with the ID remapped. Building the second frame table is just
    -- ordinary Lua table construction copying the fields it needs.
    if frame.id == 0x300 then
        local remapped = {
            id = 0x301, ext = frame.ext, rtr = frame.rtr, fd = frame.fd,
            brs = frame.brs, esi = frame.esi, len = frame.len, data = frame.data,
        }
        return {
            "can1",                                   -- unmodified frame -> can1
            { channel = "can2", frame = remapped },    -- remapped frame  -> can2
        }
    end

    -- Default: pass everything else through to can1 unmodified.
    return "can1"
end

-- Called once when this channel stops (Ctrl+C, or the source daemon
-- disconnecting). Errors here are only logged, not acted on.
function finalize()
    print(string.format("gateway_example.lua: finalize (%d frames seen)",
                         frame_count))
    return 0
end
