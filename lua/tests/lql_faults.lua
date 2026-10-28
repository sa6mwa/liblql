local core = require("lql.core")
local cases = {}
local function case(name, run)
  cases[#cases + 1] = {name = name, run = run}
end
local function acquire(name, fn)
  case(name, function()
    local c = assert(core.new())
    local s = assert(c:selector_parse('/x=1'))
    arm()
    return fn(c, s)
  end)
end
acquire("selector_parse", function(c) return c:selector_parse('/x=1') end)
acquire("selector_parse_or", function(c) return c:selector_parse_or('/x=1 /y=2') end)
acquire("selector_parse_json", function(c)
  return c:selector_parse_json('{"eq":{"field":"/x","value":"1"}}')
end)
acquire("selector_build_all", function(c) return c:selector_build_all() end)
acquire("selector_build_compound", function(c, s)
  return c:selector_build_compound("and", {s, s})
end)
acquire("selector_build_not", function(c, s) return c:selector_build_not(s) end)
acquire("selector_build_string", function(c)
  return c:selector_build_string("icontains", {field = "/x", any = {"1", "2"}})
end)
acquire("selector_build_range", function(c)
  return c:selector_build_range({field = "/x", gte = {kind = "number", number_text = "1"}})
end)
acquire("selector_build_date", function(c)
  return c:selector_build_date({field = "/date", since = "today", since_kind = "today"})
end)
acquire("selector_build_in", function(c)
  return c:selector_build_in({field = "/x", any = {"1", "2"}})
end)
acquire("selector_build_exists", function(c) return c:selector_build_exists("/x") end)
acquire("projection_parse", function(c) return c:projection_parse({"/x", "/y"}) end)
acquire("mutation_parse", function(c) return c:mutation_parse({"/x=2", "/y=true"}) end)
acquire("mutation_callbacks", function(c)
  return c:mutation_parse({"time:/when=NOW", "textfile:/x=virtual"}, {
    time_now = function() return 1700000000 end,
    file_value_open = function() return function() return nil end end,
    file_value_close = function() return true end,
  })
end)
acquire("selector_write_json", function(_, s) return s:write_json() end)
acquire("apply_string_transient", function(c)
  return c:apply_string_spooled('/x=1', '{"x":1}\n', {
    projection = {"/x"}, mutation = {"/y=2"},
  })
end)
acquire("apply_string_output", function(c, s)
  return c:apply_string_spooled(s, '{"x":1}\n')
end)
acquire("cursor_view", function(_, s) return s:root():string_term() end)

-- Arm inside native callbacks to cover allocations after native acquisition,
-- not just method setup. Each no-fault control must execute the chosen callback.
for _, method in ipairs({"stream_apply", "stream_apply_spooled"}) do
  for _, point in ipairs({"reader", "decision", "value", "writer", "value_writer", "range", "cancelled", "time"}) do
    if not (method == "stream_apply_spooled" and point == "range") then
      case(method .. ":" .. point, function()
        local c = assert(core.new())
        local selector = point == "time" and '/date since today' or '/x=1'
        -- The established date grammar is supplied by the public JSON parser.
        if point == "time" then
          selector = assert(c:selector_build_date({field = "/date", since = "today", since_kind = "today"}))
        end
        local data = '{"x":1,"date":"2026-10-28T12:00:00Z","pad":"' .. string.rep("a", 1000) .. '"}\n'
        local done, hit = false, false
        local function fault(where)
          if point == where and not hit then hit = true; arm() end
        end
        local options = {
          input_is_compact = true,
          output_mode = "selected_record",
          matched_only = false,
          on_decision = function() fault("decision"); return true end,
          on_value = function(value)
            saved_value = value
            fault("value")
            if point == "value_writer" then
              return value:write_to(function() fault("value_writer"); return string.rep("b", 1000) end)
            end
            return true
          end,
          writer = function() fault("writer"); return string.rep("b", 1000) end,
          range_writer = function(_, _, writer)
            saved_writer = writer
            fault("range")
            return writer:write(data)
          end,
          cancelled = function() fault("cancelled"); return false end,
          time_now = function() fault("time"); return 1793188800 end,
        }
        local result, err = c[method](c, function()
          if done then return nil end
          done = true
          fault("reader")
          return data
        end, selector, options)
        -- Without an allocation error, prove this fixture reached its target.
        assert(hit, "callback was not reached: " .. point)
        return result, err
      end)
    end
  end
end
case("mutation_time_callback", function()
  local c = assert(core.new())
  return c:mutation_parse({"time:/when=NOW"}, {time_now = function() arm(); return 1700000000 end})
end)
for _, point in ipairs({"open", "read"}) do
  case("mutation_file_" .. point, function()
    local c = assert(core.new())
    local mutation = assert(c:mutation_parse({"textfile:/x=virtual"}, {
      file_value_open = function()
        if point == "open" then arm() end
        local done = false
        return function()
          if done then return nil end
          done = true
          if point == "read" then arm() end
          return string.rep("payload", 100)
        end
      end,
    }))
    return c:apply_string_spooled(nil, '{}\n', {mutation = mutation, matched_only = false})
  end)
end
for _, point in ipairs({"read", "close"}) do
  case("paired_file_close_" .. point, function()
    local c = assert(core.new())
    opened_readers, closed_readers = 0, 0
    local mutation = assert(c:mutation_parse("textfile:/payload=virtual", {
      file_value_open = function()
        local done = false
        local reader = function()
          if done then return nil end
          done = true
          if point == "read" then arm() end
          return string.rep("payload", 100)
        end
        opened_readers = opened_readers + 1
        return reader
      end,
      file_value_close = function()
        closed_readers = closed_readers + 1
        if point == "close" then arm() end
        return string.rep("closed", 100)
      end,
    }))
    return c:apply_string_spooled(nil, '{}\n', {mutation = mutation})
  end)
end
return cases
