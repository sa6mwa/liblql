local lql = require("lql")
local core = require("lql.core")

local function fail(message)
  io.stderr:write(message .. "\n")
  os.exit(1)
end

local function assert_equal(got, want, message)
  if got ~= want then
    fail(message .. ": got " .. tostring(got) .. " want " .. tostring(want))
  end
end

local function assert_truthy(value, message)
  if not value then
    fail(message)
  end
  return value
end

local function assert_no_error(value, err, message)
  if err then
    fail(message .. ": " .. tostring(err.message or err.status_string or err.status))
  end
  return value
end

local client = assert_no_error(lql.new(), nil, "new client")
assert_equal(lql, core, "lql facade is lql.core")
assert_equal(lql.core, core, "lql.core self reference")
assert_truthy(type(client:version()) == "string", "client version")
assert_equal(lql.status_string(0), "ok", "module status string")
assert_equal(lql.status_string(999), "unknown", "unknown status string")
local regular_file_probe = os.tmpname()
local regular_file = assert(io.open(regular_file_probe, "wb"))
regular_file:write("{}\n")
regular_file:close()
assert_equal(lql.path_is_regular_file(regular_file_probe), true,
             "module regular file helper")
assert_equal(client:path_is_regular_file(regular_file_probe), true,
             "client regular file helper")
os.remove(regular_file_probe)
assert_equal(lql.path_is_regular_file(regular_file_probe), false,
             "missing regular file helper")

local caps = client:capabilities()
assert_equal(caps.selector_parse, true, "selector_parse capability")
assert_equal(caps.selector_inspection, true, "selector_inspection capability")
assert_equal(caps.apply_string_spooled, true, "apply_string_spooled capability")
assert_equal(caps.execute_string, nil, "stale execute_string capability removed")
assert_equal(caps.filter_file_spooled, true, "filter_file_spooled capability")
assert_equal(caps.rewrite_file_inline_spooled, true,
             "rewrite_file_inline_spooled capability")
assert_equal(caps.path_is_regular_file, true, "path_is_regular_file capability")
assert_equal(caps.projection_parse, true, "projection_parse capability")
assert_equal(caps.mutation_parse, true, "mutation_parse capability")
assert_equal(client.execute_string, nil, "stale execute_string facade removed")
assert_equal(client.execute_file, nil, "stale execute_file facade removed")
assert_equal(client.apply_file, nil, "stale apply_file facade removed")

local selector = assert_no_error(client:selector_parse('/status="open"'), nil,
                                 "selector parse")
assert_equal(selector:is_empty(), false, "non-empty selector")
assert_equal(selector:capabilities().eq, true, "selector eq capability")
assert_equal(client.__gc, nil, "client finalizer is not callable")
assert_equal(selector.__gc, nil, "selector finalizer is not callable")
assert_equal(client:selector_capabilities(selector).eq, true,
             "client selector capability from userdata")
assert_equal(client:selector_capabilities('/status="open"').eq, true,
             "client selector capability from string")

local array_in = assert_no_error(
  client:selector_parse_json('{"in":{"field":"/tags[]","any":["red","blue"]}}'),
  nil,
  "array in selector JSON parse")
local array_in_root = array_in:root()
assert_equal(array_in_root:kind(), "in", "array in root kind")
local array_in_term = assert_no_error(array_in_root:in_term(), nil,
                                      "array in term view")
assert_equal(array_in_term.field, "/tags/[]", "array in wildcard field")
assert_equal(array_in_term.any_count, 2, "array in any count")
assert_equal(array_in_root:in_term_any(1), "red", "array in first any")
assert_equal(array_in_root:in_term_any(2), "blue", "array in second any")
local array_in_result = assert_no_error(
  client:apply_string_spooled(array_in,
    '{"id":"plain","tags":["red"]}\n' ..
    '{"id":"match","tags":["blue"]}\n'), nil, "array in apply")
assert_equal(array_in_result.records_matched, 2, "array in matches array values")
local plain_in = assert_no_error(
  client:selector_build_in({field = "/tags", any = {"red", "blue"}}), nil,
  "plain in builder")
local plain_in_result = assert_no_error(
  client:apply_string_spooled(plain_in,
    '{"id":"plain","tags":["red"]}\n' ..
    '{"id":"match","tags":["blue"]}\n'), nil, "plain in apply")
assert_equal(plain_in_result.records_matched, 0,
             "plain in does not scan array values")

local built_eq = assert_no_error(
  client:selector_build_string("eq", {field = "/status", value = "open"}), nil,
  "string selector builder")
-- Builder terms may be dynamic tables. Values borrowed from one __index lookup
-- must remain rooted while a later lookup collects temporary strings.
local dynamic_builder_term = setmetatable({}, {
  __index = function(_, key)
    if key == "field" then
      return "/" .. string.rep("dynamic", 128)
    end
    collectgarbage("collect")
    if key == "value" then
      return "open"
    end
  end,
})
local dynamic_built_eq = assert_no_error(
  client:selector_build_string("eq", dynamic_builder_term), nil,
  "dynamic string selector builder")
assert_equal(dynamic_built_eq:root():string_term().value, "open",
             "dynamic builder string view")
local built_exists = assert_no_error(client:selector_build_exists("/meta/etag"),
                                     nil, "exists selector builder")
local built_or = assert_no_error(
  client:selector_build_compound("or", {built_eq, built_exists}), nil,
  "compound selector builder")
local built_not = assert_no_error(client:selector_build_not(built_eq), nil,
                                  "not selector builder")
assert_equal(built_or:root():kind(), "or", "compound selector root kind")
assert_equal(built_or:root():child_count(), 2, "compound child count")
assert_equal(built_or:root():child(1):kind(), "eq", "compound first child")
assert_equal(built_or:root():child(2):exists_path(), "/meta/etag",
             "exists node path")
assert_equal(built_not:root():child(1):string_term().value, "open",
             "not child string view")

local built_string_any = assert_no_error(client:selector_build_string("icontains", {
  field = "/labels/*", any = {"prod", "stage"}, ignore_case = true,
}), nil, "string any builder")
local string_any_root = built_string_any:root()
assert_equal(string_any_root:string_term().ignore_case, true,
             "string any ignore-case view")
assert_equal(string_any_root:string_term_any(2), "stage", "string any view")

local built_range = assert_no_error(client:selector_build_range({
  field = "/progress", gte = {kind = "number", number = 42, number_text = "42"},
}), nil, "range selector builder")
assert_equal(built_range:root():range_term().gte.number_text, "42",
             "range number view")
local built_huge_range = assert_no_error(client:selector_build_range({
  field = "/huge", gt = {kind = "number", number_text = "1e400"},
}), nil, "huge range selector builder")
assert_equal(built_huge_range:root():range_term().gt.number_text, "1e400",
             "huge range number view")
local built_date = assert_no_error(client:selector_build_date({
  field = "/timestamp", since = "today", since_kind = "today",
}), nil, "date selector builder")
local built_date_term = built_date:root():date_term()
assert_equal(built_date_term.since, "today", "date term view")
assert_equal(built_date_term.since_kind, "today", "date term classification")
local rebuilt_date, rebuilt_date_err = client:selector_build_date(built_date_term)
rebuilt_date = assert_no_error(rebuilt_date, rebuilt_date_err,
                                "date term reconstruction")
assert_equal(rebuilt_date:root():date_term().since_kind, "today",
             "reconstructed date term classification")
local built_all = assert_no_error(client:selector_build_all(), nil,
                                  "all selector builder")
assert_equal(built_all:is_empty(), true, "all selector is empty")
local built_json = assert_no_error(built_or:write_json(), nil,
                                   "selector JSON serialization")
local built_roundtrip = assert_no_error(client:selector_parse_json(built_json), nil,
                                        "selector JSON roundtrip")
assert_equal(built_roundtrip:root():kind(), "or", "roundtrip selector root")

local stream_input = '{"id":1,"status":"closed"}\n{"id":2,"status":"open"}\n'
local stream_offset = 1
local stream_reads = 0
local stream_written = {}
local stream_values = {}
local stream_decisions = {}
local expired_stream_value = nil
local expired_stream_writer = nil
local stream_result = assert_no_error(client:stream_apply(function(capacity)
  stream_reads = stream_reads + 1
  if stream_offset > #stream_input then
    return nil
  end
  local next_offset = math.min(#stream_input, stream_offset + math.min(capacity, 5) - 1)
  local chunk = stream_input:sub(stream_offset, next_offset)
  stream_offset = next_offset + 1
  return chunk
end, '/status="open"', {
  input_is_compact = true,
  output_mode = "selected_record",
  matched_only = true,
  range_writer = function(offset, length, writer)
    expired_stream_writer = writer
    return writer:write(stream_input:sub(offset + 1, offset + length))
  end,
  writer = function(chunk)
    table.insert(stream_written, chunk)
  end,
  on_decision = function(index, matched)
    table.insert(stream_decisions, {index, matched})
  end,
  on_value = function(value)
    expired_stream_value = value
    assert_equal(value:size(), #'{"id":2,"status":"open"}',
                 "stream value byte size")
    return value:write_to(function(chunk)
      table.insert(stream_values, chunk)
    end)
  end,
}), nil, "true streaming apply")
assert_truthy(stream_reads > 2, "stream reader is called incrementally")
assert_equal(stream_result.records_seen, 2, "stream records seen")
assert_equal(stream_result.records_matched, 1, "stream records matched")
assert_equal(table.concat(stream_written), '{"id":2,"status":"open"}\n',
             "stream selected output")
assert_equal(table.concat(stream_values), '{"id":2,"status":"open"}',
             "stream value output")
assert_equal(stream_decisions[1][1], 0, "stream first decision index")
assert_equal(stream_decisions[2][2], true, "stream second decision match")
assert_equal(pcall(function() return expired_stream_value:size() end), false,
             "stream values expire after their callback")
assert_equal(pcall(function() return expired_stream_writer:write("x") end), false,
             "stream writers expire after their callback")

local spooled_offset = 1
local spooled_written = {}
local spooled_result = assert_no_error(client:stream_apply_spooled(function(capacity)
  if spooled_offset > #stream_input then
    return nil
  end
  local next_offset = math.min(#stream_input, spooled_offset + capacity - 1)
  local chunk = stream_input:sub(spooled_offset, next_offset)
  spooled_offset = next_offset + 1
  return chunk
end, '/status="open"', {
  output_mode = "selected_record",
  matched_only = true,
  writer = function(chunk)
    table.insert(spooled_written, chunk)
  end,
}), nil, "spooled streaming apply")
assert_equal(spooled_result.records_matched, 1, "spooled stream records matched")
assert_equal(table.concat(spooled_written), '{"id":2,"status":"open"}\n',
             "spooled stream selected output")
local rejected_output, rejected_output_err = client:stream_apply_spooled(function()
  return stream_input
end, '/status="open"', {
  output_mode = "selected_record",
  matched_only = true,
  writer = function()
    return nil, {status = 8, message = "sink failed"}
  end,
})
if rejected_output ~= nil or not rejected_output_err or
   rejected_output_err.status ~= 8 or
   rejected_output_err.message ~= "sink failed" then
  fail("stream writer must propagate nil/error output failures")
end

-- A reader's nil is EOF only without a structured companion error. This is
-- also the result shape produced by callback-backed file handles below.
local rejected_reader, rejected_reader_err = client:stream_apply_spooled(function()
  return nil, {status = 8, message = "read failed"}
end, nil)
if rejected_reader ~= nil or not rejected_reader_err or
   rejected_reader_err.status ~= 8 or
   rejected_reader_err.message ~= "read failed" then
  fail("stream reader must propagate nil/error input failures")
end

-- Error tables are data, not callback objects: their fields must be read
-- without invoking a hostile __index metamethod after the callback returns.
local hostile_error_indexed = 0
local hostile_error = setmetatable({}, {
  __index = function()
    hostile_error_indexed = hostile_error_indexed + 1
    error("error table __index must not run")
  end,
})
local hostile_error_result, hostile_error_result_err = client:stream_apply_spooled(
  function()
    return nil, hostile_error
  end, nil)
if hostile_error_result ~= nil or not hostile_error_result_err or
   hostile_error_result_err.status_string ~= "callback error" or
   hostile_error_indexed ~= 0 then
  fail("hostile callback error tables must return a structured failure")
end

-- Option and builder getters execute under protected lookup, so they cannot
-- jump across bridge/root cleanup after the facade has acquired ownership.
local getter_reader_weak = setmetatable({}, {__mode = "v"})
do
  local getter_reader = function()
    return nil
  end
  getter_reader_weak[1] = getter_reader
  local getter_options = setmetatable({}, {
    __index = function()
      error("stream option getter failed")
    end,
  })
  local getter_result, getter_err = client:stream_apply(getter_reader, nil,
                                                         getter_options)
  if getter_result ~= nil or not getter_err or
     getter_err.status_string ~= "callback error" then
    fail("raising stream option getter must return a structured failure")
  end
end
collectgarbage("collect")
collectgarbage("collect")
assert_equal(getter_reader_weak[1], nil,
             "raising stream option getter must release reader references")

local raising_builder = setmetatable({}, {
  __index = function(_, key)
    if key == "field" then
      return "/" .. string.rep("builder", 128)
    end
    error("selector builder getter failed")
  end,
})
local raising_builder_result, raising_builder_err =
  client:selector_build_string("eq", raising_builder)
if raising_builder_result ~= nil or not raising_builder_err or
   raising_builder_err.status_string ~= "callback error" then
  fail("raising selector builder getter must return a structured failure")
end

-- Forwarding Lua's normal file:read result must preserve its string I/O
-- failure rather than treating nil, message, errno as EOF.
local directory_reader = assert(io.open(".", "rb"))
local file_reader_result, file_reader_err = client:stream_apply_spooled(
  function(capacity)
    return directory_reader:read(capacity)
  end, nil)
directory_reader:close()
if file_reader_result ~= nil or not file_reader_err or
   file_reader_err.status_string ~= "I/O error" or
   not file_reader_err.message or #file_reader_err.message == 0 then
  fail("stream reader must propagate standard Lua file I/O failures")
end

local rejected_decision, rejected_decision_err = client:stream_apply_spooled(function()
  return '{"id":1}\n'
end, nil, {
  on_decision = function()
    return nil, {status = 8, message = "decision failed"}
  end,
})
if rejected_decision ~= nil or not rejected_decision_err or
   rejected_decision_err.status ~= 8 or
   rejected_decision_err.message ~= "decision failed" then
  fail("decision callback must propagate nil/error failures")
end

local rejected_cancelled, rejected_cancelled_err = client:stream_apply_spooled(function()
  return '{"id":1}\n'
end, nil, {
  cancelled = function()
    return nil, {status = 8, message = "cancellation probe failed"}
  end,
})
if rejected_cancelled ~= nil or not rejected_cancelled_err or
   rejected_cancelled_err.status ~= 8 or
   rejected_cancelled_err.message ~= "cancellation probe failed" then
  fail("cancellation callback must propagate nil/error failures")
end

-- A clock failure is terminal before the first reader or writer invocation;
-- falling back to the host clock would process data without caller authority.
local failed_clock_reads = 0
local failed_clock_writes = 0
local failed_clock_sent = false
local failed_clock, failed_clock_err = client:stream_apply_spooled(function()
  failed_clock_reads = failed_clock_reads + 1
  if failed_clock_sent then
    return nil
  end
  failed_clock_sent = true
  return '{"timestamp":"2024-03-01T00:00:00Z"}\n' ..
         '{"timestamp":"2024-03-02T00:00:00Z"}\n' ..
         '{"timestamp":"2024-03-03T00:00:00Z"}\n'
end, built_date, {
  output_mode = "selected_record",
  matched_only = true,
  time_now = function()
    error("clock failed")
  end,
  writer = function()
    failed_clock_writes = failed_clock_writes + 1
    return true
  end,
})
if failed_clock ~= nil or not failed_clock_err or
   failed_clock_err.status_string ~= "callback error" or
   not failed_clock_err.message:find("clock failed", 1, true) then
  fail("clock callback must preserve its structured failure")
end
assert_equal(failed_clock_reads, 0,
             "clock failure must stop before reading unbounded input")
assert_equal(failed_clock_writes, 0,
             "clock failure must stop before emitting output")

local structured_clock, structured_clock_err = client:stream_apply_spooled(function()
  return nil
end, built_date, {
  time_now = function()
    return nil, {status = 8, message = "structured clock failed"}
  end,
})
if structured_clock ~= nil or not structured_clock_err or
   structured_clock_err.status ~= 8 or
   structured_clock_err.message ~= "structured clock failed" then
  fail("clock callback must propagate nil/error failures")
end
local invalid_limit, invalid_limit_err = client:stream_apply(function()
  return nil
end, nil, {limits = {max_records = -1}})
if invalid_limit ~= nil or not invalid_limit_err or
   invalid_limit_err.status_string ~= "invalid argument" then
  fail("expected negative stream limit to fail")
end
local oversized_limit, oversized_limit_err = client:stream_apply_spooled(
  function()
    return nil
  end, nil, {limits = {max_records = 4294967296}})
if string.packsize("T") < 8 then
  if oversized_limit ~= nil or not oversized_limit_err or
     oversized_limit_err.status_string ~= "invalid argument" then
    fail("stream limits must reject values larger than target size_t")
  end
elseif oversized_limit == nil or oversized_limit_err then
  fail("representable stream limits must remain accepted on 64-bit targets")
end

-- Callback methods return nil, error on output failure.  Range and value
-- callbacks must preserve that status instead of acknowledging failed output.
local rejected_range, rejected_range_err = client:stream_apply(function()
  return stream_input
end, '/status="open"', {
  input_is_compact = true,
  output_mode = "selected_record",
  matched_only = true,
  range_writer = function(offset, length, writer)
    return writer:write(stream_input:sub(offset + 1, offset + length))
  end,
  writer = function()
    return false
  end,
})
if rejected_range ~= nil or not rejected_range_err or
   rejected_range_err.status_string ~= "callback error" then
  fail("range writer must propagate rejected output")
end

local rejected_value, rejected_value_err = client:stream_apply(function()
  return stream_input
end, '/status="open"', {
  input_is_compact = true,
  output_mode = "selected_record",
  matched_only = true,
  range_writer = function(offset, length, writer)
    return writer:write(stream_input:sub(offset + 1, offset + length))
  end,
  writer = function()
    return true
  end,
  on_value = function(value)
    return value:write_to(function()
      return false
    end)
  end,
})
if rejected_value ~= nil or not rejected_value_err or
   rejected_value_err.status_string ~= "callback error" then
  fail("value callback must propagate rejected output")
end

-- Invalid limits must return through normal cleanup after callback references
-- have been acquired.  The weak reference proves the reader is not retained.
local invalid_limits_weak = setmetatable({}, {__mode = "v"})
do
  local invalid_limits_reader = function()
    return nil
  end
  invalid_limits_weak[1] = invalid_limits_reader
  local invalid_limits_result, invalid_limits_err = client:stream_apply(
    invalid_limits_reader, nil, {limits = false})
  if invalid_limits_result ~= nil or not invalid_limits_err or
     invalid_limits_err.status_string ~= "invalid argument" then
    fail("non-table stream limits must return an argument error")
  end
end
collectgarbage("collect")
collectgarbage("collect")
assert_equal(invalid_limits_weak[1], nil,
             "invalid limits must release stream callback references")

-- Invalid selectors return normal facade errors after callbacks are acquired;
-- the weak reference proves that cleanup releases the reader closure.
local invalid_selector_weak = setmetatable({}, {__mode = "v"})
do
  local invalid_selector_reader = function()
    return nil
  end
  invalid_selector_weak[1] = invalid_selector_reader
  local invocation = {pcall(function()
    return client:stream_apply(invalid_selector_reader, {})
  end)}
  if not invocation[1] or invocation[2] ~= nil or not invocation[3] or
     invocation[3].status_string ~= "invalid argument" then
    fail("invalid stream selector must return a structured argument error")
  end
end
collectgarbage("collect")
collectgarbage("collect")
assert_equal(invalid_selector_weak[1], nil,
             "invalid selector must release stream callback references")

-- Userdata transforms remain C-owned for the duration of stream processing,
-- even if a re-entrant reader drops their last Lua-table references.
local transform_options = {
  projection = assert_no_error(client:projection_parse({"/id"}), nil,
                               "stream transform projection"),
  mutation = assert_no_error(client:mutation_parse({"/seen=true"}), nil,
                             "stream transform mutation"),
  output_mode = "projection_then_mutation",
  writer = function()
    return true
  end,
}
local transform_input = '{"id":1}\n'
local transform_read = false
local transform_result = assert_no_error(client:stream_apply_spooled(function()
  if transform_read then
    return nil
  end
  transform_read = true
  transform_options.projection = nil
  transform_options.mutation = nil
  collectgarbage("collect")
  return transform_input
end, nil, transform_options), nil, "stream transform rooting")
assert_equal(transform_result.records_seen, 1,
             "stream transform remains usable after Lua collection")

-- A retained mutation keeps its originating coroutine and callbacks alive for
-- application, then releases both once the userdata is unreachable.
local coroutine_mutation = nil
local coroutine_weak = setmetatable({}, {__mode = "v"})
do
  local mutation_thread = coroutine.create(function()
    coroutine_mutation = assert_no_error(client:mutation_parse({
      'file:/payload="coroutine payload"',
    }, {
      file_value_open = function(path)
        assert_equal(path, "coroutine payload", "coroutine file callback path")
        local sent = false
        return function()
          if sent then
            return nil
          end
          sent = true
          return "coroutine payload"
        end
      end,
    }), nil, "coroutine mutation parse")
  end)
  coroutine_weak[1] = mutation_thread
  assert_truthy(coroutine.resume(mutation_thread), "coroutine mutation parse")
end
collectgarbage("collect")
collectgarbage("collect")
assert_truthy(coroutine_weak[1] ~= nil,
              "retained mutation must retain originating coroutine")
local coroutine_result = assert_no_error(client:apply_string_spooled(nil,
  '{"id":1}\n', {mutation = coroutine_mutation}), nil,
  "coroutine mutation application")
assert_equal(coroutine_result.output,
             '{"id":1,"payload":"coroutine payload"}\n',
             "coroutine mutation output")
coroutine_mutation = nil
collectgarbage("collect")
collectgarbage("collect")
assert_equal(coroutine_weak[1], nil,
             "mutation finalization must release originating coroutine")

-- A yielded coroutine that holds the only mutation reference forms a cycle.
-- The mutation's uservalue owns that cycle, so it must remain collectible.
local orphan_mutation_weak = setmetatable({}, {__mode = "v"})
do
  local orphan_mutation_thread = coroutine.create(function()
    local orphan_mutation = assert_no_error(client:mutation_parse({
      'file:/payload="orphan payload"',
    }, {
      file_value_open = function()
        return function()
          return nil
        end
      end,
    }), nil, "orphan coroutine mutation parse")
    assert_truthy(orphan_mutation ~= nil, "orphan mutation exists")
    coroutine.yield()
  end)
  orphan_mutation_weak[1] = orphan_mutation_thread
  assert_truthy(coroutine.resume(orphan_mutation_thread),
                "orphan coroutine mutation parse")
end
collectgarbage("collect")
collectgarbage("collect")
assert_equal(orphan_mutation_weak[1], nil,
             "orphan callback mutation cycle must be collectible")

local result = assert_no_error(
  client:apply_string_spooled(selector,
                        '{"status":"closed","id":1}\n' ..
                        '{"status":"open","id":2}\n'),
  nil,
  "apply string")
assert_equal(result.records_seen, 2, "records seen")
assert_equal(result.records_matched, 1, "records matched")
assert_equal(result.stopped_early, false, "string stopped early")
assert_equal(result.stop_reason, 0, "string stop reason")
assert_equal(result.output, '{"status":"open","id":2}\n', "selected output")

local selected_with_unmatched = assert_no_error(
  client:apply_string_spooled('/status="open"',
                        '{"status":"closed","id":1}\n' ..
                        '{"status":"open","id":2}\n',
                        {matched_only = false}),
  nil,
  "apply selected records with unmatched")
assert_equal(selected_with_unmatched.output,
             '{"status":"closed","id":1}\n{"status":"open","id":2}\n',
             "selected-record unmatched output")

local count = assert_no_error(
  client:apply_string_spooled('/status="open"',
                        '{"status":"open"}\n{"status":"closed"}\n',
                        {count = true}),
  nil,
  "apply string count")
assert_equal(count.records_seen, 2, "count records seen")
assert_equal(count.records_matched, 1, "count records matched")
assert_equal(count.output, nil, "count has no output")

local all_records = assert_no_error(
  client:apply_string_spooled(nil, '{"status":"open"}\n{"status":"closed"}\n'),
  nil,
  "apply nil selector")
assert_equal(all_records.records_seen, 2, "nil selector records seen")
assert_equal(all_records.records_matched, 2, "nil selector records matched")

local projection = assert_no_error(client:projection_parse({"/status"}), nil,
                                   "projection parse")
assert_equal(projection:path_count(), 1, "projection path count")
assert_equal(projection:path(1), "/status", "projection path")
local bad_projection, bad_projection_err = client:projection_parse({1})
if bad_projection ~= nil or not bad_projection_err or
   bad_projection_err.status_string ~= "invalid argument" then
  fail("expected structured projection string-list error")
end
local projected = assert_no_error(
  client:apply_string_spooled(nil,
                        '{"status":"open","id":1}\n' ..
                        '{"status":"closed","id":2}\n',
                        {projection = projection}),
  nil,
  "apply projection")
assert_equal(projected.output, '{"status":"open"}\n{"status":"closed"}\n',
             "projection output")
local projected_with_unmatched = assert_no_error(
  client:apply_string_spooled('/status="open"',
                        '{"status":"closed","id":1}\n' ..
                        '{"status":"open","id":2}\n',
                        {projection = projection, matched_only = false}),
  nil,
  "apply projection with unmatched")
assert_equal(projected_with_unmatched.output,
             '{"status":"closed"}\n{"status":"open"}\n',
             "projection unmatched output")

local mutation = assert_no_error(client:mutation_parse({"/status=ready"}), nil,
                                 "mutation parse")
assert_equal(mutation:count(), 1, "mutation count")
local bad_mutation, bad_mutation_err = client:mutation_parse({{}})
if bad_mutation ~= nil or not bad_mutation_err or
   bad_mutation_err.status_string ~= "invalid argument" then
  fail("expected structured mutation string-list error")
end
local mutated = assert_no_error(
  client:apply_string_spooled('/status="open"',
                        '{"status":"closed","id":1}\n' ..
                        '{"status":"open","id":2}\n',
                        {mutation = mutation, matched_only = true}),
  nil,
  "apply mutation")
assert_equal(mutated.output, '{"status":"ready","id":2}\n',
             "mutation matched-only output")

local projected_mutated = assert_no_error(
  client:apply_string_spooled(nil, '{"status":"open","id":1}\n',
                        {projection = {"/status"}, mutation = {"/seen=true"}}),
  nil,
  "apply projection then mutation")
assert_equal(projected_mutated.output, '{"status":"open","seen":true}\n',
             "projection then mutation output")

local file_fixture = os.tmpname()
local file = assert(io.open(file_fixture, "wb"))
file:write("héllo")
file:close()
local file_mutation = assert_no_error(
  client:mutation_parse({"textfile:/content=" .. file_fixture},
                        {enable_file_mutations = true}),
  nil,
  "file mutation parse")
local file_mutated = assert_no_error(
  client:apply_string_spooled(nil, "{}\n", {mutation = file_mutation}),
  nil,
  "apply file-backed mutation")
assert_equal(file_mutated.output, '{"content":"héllo"}\n',
             "file-backed mutation output")
os.remove(file_fixture)

local virtual_open_count = 0
local virtual_mutation = assert_no_error(client:mutation_parse({
  'file:/payload="virtual:payload"',
}, {
  file_value_open = function(path)
    assert_equal(path, "virtual:payload", "virtual file source path")
    virtual_open_count = virtual_open_count + 1
    local content = string.char(0, 1, 2)
    local offset = 1
    return function(capacity)
      if offset > #content then
        return nil
      end
      local next_offset = math.min(#content, offset + capacity - 1)
      local chunk = content:sub(offset, next_offset)
      offset = next_offset + 1
      return chunk
    end
  end,
}), nil, "virtual file mutation parse")
local virtual_mutated = assert_no_error(
  client:apply_string_spooled(nil, "{}\n", {mutation = virtual_mutation}),
  nil, "apply virtual file-backed mutation")
assert_equal(virtual_mutated.output, '{"payload":"AAEC"}\n',
             "virtual file-backed mutation output")
assert_equal(virtual_open_count, 2, "virtual file source opens for each pass")

local rejected_file_mutation = assert_no_error(client:mutation_parse({
  'file:/payload="broken"',
}, {
  file_value_open = function()
    return function()
      return nil, {status = 8, message = "file read failed"}
    end
  end,
}), nil, "failing virtual file mutation parse")
local rejected_file, rejected_file_err = client:apply_string_spooled(
  nil, "{}\n", {mutation = rejected_file_mutation})
if rejected_file ~= nil or not rejected_file_err or
   rejected_file_err.status ~= 8 or
   rejected_file_err.message ~= "file read failed" then
  fail("file reader must propagate nil/error input failures")
end

local rejected_open_mutation = assert_no_error(client:mutation_parse({
  'file:/payload="unavailable"',
}, {
  file_value_open = function()
    return nil, {status = 8, message = "file open failed"}
  end,
}), nil, "failing virtual file opener mutation parse")
local rejected_open, rejected_open_err = client:apply_string_spooled(
  nil, "{}\n", {mutation = rejected_open_mutation})
if rejected_open ~= nil or not rejected_open_err or
   rejected_open_err.status ~= 8 or
   rejected_open_err.message ~= "file open failed" then
  fail("file opener must propagate nil/error failures")
end

-- A callback failure ends only its current application. A retained mutation
-- must retry its callbacks when a later public application uses it.
local retry_open_attempts = 0
local retry_mutation = assert_no_error(client:mutation_parse({
  'file:/payload="retry"',
}, {
  file_value_open = function()
    retry_open_attempts = retry_open_attempts + 1
    if retry_open_attempts == 1 then
      return nil, {status = 8, message = "temporarily unavailable"}
    end
    local sent = false
    return function()
      if sent then
        return nil
      end
      sent = true
      return "recovered"
    end
  end,
}), nil, "retryable virtual file mutation parse")
local retry_first, retry_first_err = client:apply_string_spooled(
  nil, "{}\n", {mutation = retry_mutation})
if retry_first ~= nil or not retry_first_err or
   retry_first_err.message ~= "temporarily unavailable" then
  fail("first retryable mutation application must expose callback failure")
end
local retry_second, retry_second_err = client:apply_string_spooled(
  nil, "{}\n", {mutation = retry_mutation})
if retry_second == nil or retry_second_err or retry_open_attempts < 2 or
   not retry_second.output:find('"payload":"recovered"', 1, true) then
  fail("retained mutation must retry callbacks after a failed application")
end

-- Mutation expression strings are rooted independently from their mutable
-- input table while parsing can invoke time_now. Exercise both the retained
-- handle and temporary-mutation facades under forced collection.
local rooted_expressions = {
  "time:/when=NOW",
  '/payload="' .. string.rep("x", 1024) .. '"',
}
local rooted_mutation, rooted_mutation_err = client:mutation_parse(
  rooted_expressions, {
    time_now = function()
      rooted_expressions[2] = nil
      collectgarbage("collect")
      return 1709251200
    end,
  })
if rooted_mutation == nil or rooted_mutation_err then
  fail("persistent mutation parsing must retain expression strings")
end
assert_equal(rooted_mutation:count(), 2,
             "persistent mutation keeps every rooted expression")

local rejected_mutation_clock, rejected_mutation_clock_err =
  client:mutation_parse({"time:/when=NOW"}, {
    time_now = function()
      return nil, {status = 8, message = "mutation clock failed"}
    end,
  })
if rejected_mutation_clock ~= nil or not rejected_mutation_clock_err or
   rejected_mutation_clock_err.status ~= 8 or
   rejected_mutation_clock_err.message ~= "mutation clock failed" then
  fail("mutation clock must propagate nil/error failures")
end

-- A terminal callback failure is the authoritative result, even if the
-- parser encounters a later malformed expression. Cover both persistent and
-- temporary mutation construction, which use separate cleanup paths.
local callback_precedence, callback_precedence_err = client:mutation_parse({
  "time:/when=NOW",
  "bad{",
}, {
  time_now = function()
    return nil, {status = 8, message = "clock unavailable"}
  end,
})
if callback_precedence ~= nil or not callback_precedence_err or
   callback_precedence_err.status ~= 8 or
   callback_precedence_err.message ~= "clock unavailable" then
  fail("persistent mutation parsing must preserve terminal callback failures")
end
local temporary_callback_precedence, temporary_callback_precedence_err =
  client:apply_string_spooled(nil, "{}\n", {
    mutation = {"time:/when=NOW", "bad{"},
    time_now = function()
      return nil, {status = 8, message = "temporary clock unavailable"}
    end,
  })
if temporary_callback_precedence ~= nil or
   not temporary_callback_precedence_err or
   temporary_callback_precedence_err.status ~= 8 or
   temporary_callback_precedence_err.message ~= "temporary clock unavailable" then
  fail("temporary mutation parsing must preserve terminal callback failures")
end

local base_dir_fixture = os.tmpname()
local base_dir_file = assert(io.open(base_dir_fixture, "wb"))
base_dir_file:write("base-dir payload")
base_dir_file:close()
local base_dir, base_name = base_dir_fixture:match("^(.*)/([^/]+)$")
local base_dir_options = {
  enable_file_mutations = true,
  file_value_base_dir = base_dir,
}
base_dir_options.time_now = function()
  base_dir_options.file_value_base_dir = nil
  collectgarbage("collect")
  return 1709251200
end
local base_dir_mutation, base_dir_mutation_err = client:mutation_parse({
  "time:/when=NOW",
  "textfile:/payload=" .. base_name,
}, base_dir_options)
if base_dir_mutation == nil or base_dir_mutation_err then
  fail("mutation parsing must retain file_value_base_dir through callbacks")
end
local base_dir_result, base_dir_result_err = client:apply_string_spooled(
  nil, "{}\n", {mutation = base_dir_mutation})
if base_dir_result == nil or base_dir_result_err or
   not base_dir_result.output:find('"payload":"base-dir payload"', 1, true) then
  fail("mutation must use the rooted file_value_base_dir")
end

-- Root the exact first base-directory value through every later options lookup,
-- even when no callback bridge is configured.
local dynamic_base_dir_options = setmetatable({
  enable_file_mutations = true,
}, {
  __index = function(_, key)
    if key == "file_value_base_dir" then
      return base_dir .. string.rep("/.", 128)
    end
    if key == "file_value_open" or key == "time_now" then
      collectgarbage("collect")
    end
  end,
})
local dynamic_base_dir_mutation, dynamic_base_dir_mutation_err =
  client:mutation_parse({"textfile:/payload=" .. base_name},
                        dynamic_base_dir_options)
if dynamic_base_dir_mutation == nil or dynamic_base_dir_mutation_err then
  fail("dynamic base directory mutation parse")
end
local dynamic_base_dir_result, dynamic_base_dir_result_err =
  client:apply_string_spooled(nil, "{}\n", {mutation = dynamic_base_dir_mutation})
if dynamic_base_dir_result == nil or dynamic_base_dir_result_err or
   not dynamic_base_dir_result.output:find('"payload":"base-dir payload"', 1,
                                           true) then
  fail("mutation must retain a dynamic base directory without callbacks")
end
os.remove(base_dir_fixture)

local temporary_options = {
  mutation = {
    "time:/when=NOW",
    '/payload="' .. string.rep("y", 1024) .. '"',
  },
}
temporary_options.time_now = function()
  temporary_options.mutation[2] = nil
  collectgarbage("collect")
  return 1709251200
end
local temporary_result, temporary_err = client:apply_string_spooled(
  nil, '{"id":1}\n', temporary_options)
if temporary_result == nil or temporary_err then
  fail("temporary mutation parsing must retain expression strings")
end

-- Persistent callback ownership transfers the original captured functions,
-- rather than re-reading an options table a parsing callback can mutate.
local transfer_options = {
  file_value_open = function(path)
    assert_equal(path, "transfer:payload", "captured file callback path")
    local sent = false
    return function()
      if sent then
        return nil
      end
      sent = true
      return "captured payload"
    end
  end,
}
transfer_options.time_now = function()
  transfer_options.file_value_open = nil
  collectgarbage("collect")
  return 1709251200
end
local transferred_mutation, transferred_mutation_err = client:mutation_parse({
  "time:/when=NOW",
  'file:/payload="transfer:payload"',
}, transfer_options)
if transferred_mutation == nil or transferred_mutation_err then
  fail("mutation callback transfer parse failed")
end
local transferred_result, transferred_result_err = client:apply_string_spooled(
  nil, "{}\n", {mutation = transferred_mutation})
if transferred_result == nil or transferred_result_err or
   not transferred_result.output:find('"payload":"captured payload"', 1, true) then
  fail("persistent mutation must retain originally captured callbacks")
end

-- Metatable-backed options are read exactly once per callback. A later lookup
-- may legitimately produce a different result, so the callback validated for
-- parsing must be the one owned by the retained mutation.
local captured_file_lookups = 0
local captured_clock_lookups = 0
local captured_options = setmetatable({}, {
  __index = function(_, key)
    if key == "file_value_open" then
      captured_file_lookups = captured_file_lookups + 1
      if captured_file_lookups == 1 then
        return function(path)
          assert_equal(path, "captured:payload", "metatable file callback path")
          local sent = false
          return function()
            if sent then
              return nil
            end
            sent = true
            return "captured metatable payload"
          end
        end
      end
      return nil
    end
    if key == "time_now" then
      captured_clock_lookups = captured_clock_lookups + 1
      if captured_clock_lookups == 1 then
        return function()
          return 1709251200
        end
      end
      return nil
    end
  end,
})
local captured_mutation, captured_mutation_err = client:mutation_parse({
  "time:/when=NOW",
  'file:/payload="captured:payload"',
}, captured_options)
if captured_mutation == nil or captured_mutation_err or
   captured_file_lookups ~= 1 or captured_clock_lookups ~= 1 then
  fail("mutation parsing must capture metatable callbacks exactly once")
end
local captured_result, captured_result_err = client:apply_string_spooled(
  nil, "{}\n", {mutation = captured_mutation})
if captured_result == nil or captured_result_err or
   not captured_result.output:find('"payload":"captured metatable payload"',
                                   1, true) then
  fail("retained mutation must use its initially captured metatable callbacks")
end

local time_mutation = assert_no_error(client:mutation_parse({"time:/now=NOW"}, {
  time_now = function()
    return 1709251200
  end,
}), nil, "clocked mutation parse")
local timed_mutation = assert_no_error(
  client:apply_string_spooled(nil, "{}\n", {mutation = time_mutation}),
  nil, "apply clocked mutation")
assert_equal(timed_mutation.output, '{"now":"2024-03-01T00:00:00Z"}\n',
             "clocked mutation output")

local fixture = os.tmpname()
local f = assert(io.open(fixture, "wb"))
f:write('{"status":"closed","id":1}\n{"status":"open","id":2}\n')
f:close()
local streamed = assert_no_error(
  client:filter_file_spooled('/status="open"', fixture),
  nil,
  "filter file")
assert_equal(streamed.records_seen, 2, "file records seen")
assert_equal(streamed.records_matched, 1, "file records matched")
assert_equal(streamed.stopped_early, false, "file stopped early")
assert_equal(streamed.stop_reason, 0, "file stop reason")
assert_equal(streamed.output, '{"status":"open","id":2}\n',
             "file selected output")
local file_writer = {}
local streamed_to_writer = assert_no_error(
  client:filter_file_spooled('/status="open"', fixture, {
    writer = function(chunk)
      table.insert(file_writer, chunk)
    end,
  }), nil, "filter file to Lua writer")
assert_equal(streamed_to_writer.output, nil, "file writer does not materialize output")
assert_equal(table.concat(file_writer), '{"status":"open","id":2}\n',
             "file writer output")
local rooted_output_path = os.tmpname()
local rooted_file_options = {
  output_path = rooted_output_path,
  mutation = {"time:/when=NOW", "/seen=true"},
}
rooted_file_options.time_now = function()
  rooted_file_options.output_path = nil
  collectgarbage("collect")
  return 1709251200
end
local rooted_file_result, rooted_file_err = client:filter_file_spooled(
  nil, fixture, rooted_file_options)
if rooted_file_result == nil or rooted_file_err then
  fail("filter_file_spooled must retain output_path through mutation callbacks")
end
local rooted_file = assert(io.open(rooted_output_path, "rb"))
local rooted_file_output = rooted_file:read("*a")
rooted_file:close()
os.remove(rooted_output_path)
if not rooted_file_output:find('"seen":true', 1, true) then
  fail("filter_file_spooled must write through the rooted output_path")
end
local inline_writer, inline_writer_err = client:rewrite_file_inline_spooled(
  nil, fixture, {writer = function() end})
if inline_writer ~= nil or not inline_writer_err or
   inline_writer_err.status_string ~= "invalid argument" then
  fail("expected inline rewrite to reject an external writer")
end
os.remove(fixture)

local rewrite_fixture = os.tmpname()
local rewrite = assert(io.open(rewrite_fixture, "wb"))
rewrite:write('{"status":"open","id":2}\n')
rewrite:close()
local rewrite_mutation = assert_no_error(client:mutation_parse({"/done=true"}),
                                         nil, "rewrite mutation parse")
local rewritten = assert_no_error(
  client:rewrite_file_inline_spooled('/status="open"', rewrite_fixture,
                                     {mutation = rewrite_mutation}),
  nil,
  "rewrite file inline")
assert_equal(rewritten.records_seen, 1, "rewrite records seen")
assert_equal(rewritten.stopped_early, false, "rewrite stopped early")
assert_equal(rewritten.stop_reason, 0, "rewrite stop reason")
local rewritten_file = assert(io.open(rewrite_fixture, "rb"))
local rewritten_body = rewritten_file:read("*a")
rewritten_file:close()
assert_equal(rewritten_body, '{"status":"open","id":2,"done":true}\n',
             "rewrite inline output")
os.remove(rewrite_fixture)

local invalid_selector, invalid_err = client:selector_parse('eq{bad')
if invalid_selector ~= nil or not invalid_err or invalid_err.status == 0 then
  fail("expected structured selector parse error")
end

local invalid_run, invalid_run_err =
  client:apply_string_spooled(selector, '{"status":"open"}\n[')
if invalid_run ~= nil or not invalid_run_err or invalid_run_err.status == 0 then
  fail("expected structured apply error")
end

local ok_missing_input = pcall(function()
  client:apply_string_spooled('/status="open"')
end)
if ok_missing_input then
  fail("expected apply_string_spooled missing input argument error")
end

local ok_missing_path = pcall(function()
  client:filter_file_spooled('/status="open"')
end)
if ok_missing_path then
  fail("expected filter_file_spooled missing path argument error")
end
