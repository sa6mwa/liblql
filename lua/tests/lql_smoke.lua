local lql = require("lql")

local function fail(message)
  io.stderr:write(message .. "\n")
  os.exit(1)
end

local function assert_equal(got, want, message)
  if got ~= want then
    fail(message .. ": got " .. tostring(got) .. " want " .. tostring(want))
  end
end

local function assert_no_error(value, err, message)
  if err then
    fail(message .. ": " .. (err.stderr or err.stdout or tostring(err.status)))
  end
  return value
end

local function write_file(path, data)
  local file = io.open(path, "wb")
  if not file then
    fail("failed to open " .. path)
  end
  file:write(data)
  file:close()
end

local tmp_base = os.tmpname()
local input_path = tmp_base .. ".jsonl"
local tmp_dir, tmp_name = string.match(tmp_base, "^(.*)/(.*)$")
if not tmp_dir then
  tmp_dir = "."
  tmp_name = tmp_base
end
local text_payload_name = tmp_name .. ".txt"
local bin_payload_name = tmp_name .. ".bin"
local text_payload_path = tmp_dir .. "/" .. text_payload_name
local bin_payload_path = tmp_dir .. "/" .. bin_payload_name

write_file(input_path,
           '{"status":"closed","id":"a","count":1}\n' ..
             '{"status":"open","id":"b","count":2,"state":{"old":true}}\n')
write_file(text_payload_path, 'lua\n"payload"')
write_file(bin_payload_path, string.char(0, 1, 2, 97))

if os.getenv("LQL_REQUIRE_CORE") == "1" and not lql.has_core() then
  fail("direct lql.core module was not loaded")
end

local client = lql.new()

local matched, err = client:matches_json('/status="open"', '{"status":"open"}')
matched = assert_no_error(matched, err, "matches_json open")
assert_equal(matched, true, "matches_json open")

matched, err = client:matches_json('/status="open"', '{"status":"closed"}')
matched = assert_no_error(matched, err, "matches_json closed")
assert_equal(matched, false, "matches_json closed")

local selected
selected, err = client:select_file('/status="open"', input_path,
                                  {compact = true})
selected = assert_no_error(selected, err, "select_file")
assert_equal(selected,
             '{"status":"open","id":"b","count":2,"state":{"old":true}}\n',
             "select_file output")

local decisions = {}
local result
result, err = client:query_file('/status="open"', input_path, function(decision)
  decisions[#decisions + 1] = {
    matched = decision.matched,
    index = decision.index,
    offset = decision.offset,
    size = decision.size
  }
end)
result = assert_no_error(result, err, "query_file")
assert_equal(result.candidates_seen, 2, "query_file candidates")
assert_equal(result.candidates_matched, 1, "query_file matches")
assert_equal(decisions[1].matched, false, "query_file first decision")
assert_equal(decisions[2].matched, true, "query_file second decision")

local stopped
stopped, err = client:query_file('/status="open"', input_path, function(_)
  return false
end)
stopped = assert_no_error(stopped, err, "query_file stop")
assert_equal(stopped.stopped_early, true, "query_file stop flag")
assert_equal(stopped.stop_reason, 4, "query_file callback stop reason")

local limited = assert_no_error(client:query_file('/status="open"', input_path,
                                                 function(_) end,
                                                 {max_candidates = 1}),
                                nil, "query_file max_candidates")
assert_equal(limited.candidates_seen, 1, "query_file max_candidates count")
assert_equal(limited.stopped_early, true, "query_file max_candidates stop flag")
assert_equal(limited.stop_reason, 2, "query_file max_candidates stop reason")

local callback_error_result, callback_error
callback_error_result, callback_error =
  client:query_file('/status="open"', input_path, function(_)
    error("decision callback failed")
  end)
if callback_error_result ~= nil or not callback_error or
    not string.find(callback_error.stderr or "", "decision callback failed", 1,
                    true) then
  fail("expected structured query_file callback error")
end

local source_chunks = {
  '{"status":"closed","id":"s1"}\n{"status":',
  '"open","id":"s2","count":4}\n'
}
local source_index = 1
local source_decisions = {}
result, err = client:query_source('/status="open"', function(capacity)
  local chunk = source_chunks[source_index]
  if not chunk then
    return nil
  end
  if #chunk > capacity then
    chunk = string.sub(chunk, 1, capacity)
    source_chunks[source_index] = string.sub(source_chunks[source_index],
                                             capacity + 1)
  else
    source_index = source_index + 1
  end
  return chunk
end, function(decision)
  source_decisions[#source_decisions + 1] = decision
end)
result = assert_no_error(result, err, "query_source")
assert_equal(result.candidates_seen, 2, "query_source candidates")
assert_equal(result.candidates_matched, 1, "query_source matches")
assert_equal(source_decisions[2].matched, true, "query_source second match")

local payloads = {}
local streamed_payloads = {}
local retained_payload
result, err = client:each_match_file('/status="open"', input_path,
                                    function(match)
  local chunks = {}
  retained_payload = match
  assert_no_error(match.write_json(function(chunk)
    chunks[#chunks + 1] = chunk
  end), nil, "each_match_file write_json")
  payloads[#payloads + 1] = assert_no_error(match.json(), nil,
                                            "each_match_file payload")
  streamed_payloads[#streamed_payloads + 1] = table.concat(chunks)
end)
result = assert_no_error(result, err, "each_match_file")
assert_equal(result.candidates_seen, 2, "each_match_file candidates")
assert_equal(result.candidates_matched, 1, "each_match_file matches")
assert_equal(payloads[1],
             '{"status":"open","id":"b","count":2,"state":{"old":true}}',
             "each_match_file payload")
assert_equal(streamed_payloads[1], payloads[1],
             "each_match_file streamed payload")
local late_payload, late_err = retained_payload.json()
if late_payload ~= nil or not late_err or late_err.stderr == "" then
  fail("expected expired payload handle error")
end
local late_stream, late_stream_err = retained_payload.write_json(function(_) end)
if late_stream ~= nil or not late_stream_err or late_stream_err.stderr == "" then
  fail("expected expired payload stream handle error")
end

local stream_callback_result, stream_callback_error
stream_callback_result, stream_callback_error =
  client:each_match_file('/status="open"', input_path, function(match)
    local ok, stream_err = match.write_json(function(_)
      error("payload stream failed")
    end)
    if ok ~= nil or not stream_err then
      error("expected payload stream callback error")
    end
    error(stream_err.stderr or "missing stream callback error")
  end)
if stream_callback_result ~= nil or not stream_callback_error or
    not string.find(stream_callback_error.stderr or "", "payload stream failed",
                    1, true) then
  fail("expected structured payload write_json callback error")
end

local match_limited_count = 0
local match_limited = assert_no_error(
  client:each_match_file('contains{field=/id}', input_path, function(_)
    match_limited_count = match_limited_count + 1
  end, {max_matches = 1}), nil, "each_match_file max_matches")
assert_equal(match_limited_count, 1, "each_match_file max_matches callbacks")
assert_equal(match_limited.candidates_matched, 1,
             "each_match_file max_matches matched")
assert_equal(match_limited.stopped_early, true,
             "each_match_file max_matches stop flag")
assert_equal(match_limited.stop_reason, 1,
             "each_match_file max_matches stop reason")

local match_callback_result, match_callback_error
match_callback_result, match_callback_error =
  client:each_match_file('/status="open"', input_path, function(_)
    error("match callback failed")
  end)
if match_callback_result ~= nil or not match_callback_error or
    not string.find(match_callback_error.stderr or "", "match callback failed",
                    1, true) then
  fail("expected structured each_match_file callback error")
end

source_chunks = {
  '{"status":"closed","id":"p1"}\n',
  '{"status":"open","id":"p2","count":5}\n'
}
source_index = 1
local source_payload
local retained_source_payload
result, err = client:each_match_source('/status="open"', function(_)
  local chunk = source_chunks[source_index]
  source_index = source_index + 1
  return chunk
end, function(match)
  local chunks = {}
  retained_source_payload = match
  assert_no_error(match.write_json(function(chunk)
    chunks[#chunks + 1] = chunk
  end), nil, "each_match_source write_json")
  source_payload = table.concat(chunks)
end)
result = assert_no_error(result, err, "each_match_source")
assert_equal(result.candidates_seen, 2, "each_match_source candidates")
assert_equal(result.candidates_matched, 1, "each_match_source matches")
assert_equal(source_payload, '{"status":"open","id":"p2","count":5}',
             "each_match_source payload")
local late_source_payload, late_source_err = retained_source_payload.json()
if late_source_payload ~= nil or not late_source_err or
    late_source_err.stderr == "" then
  fail("expected expired source payload handle error")
end

local bad_source_result, bad_source_error =
  client:query_source('/status="open"', function(_)
    return string.rep("x", 9000)
  end, function(_) end)
if bad_source_result ~= nil or not bad_source_error or
    not string.find(bad_source_error.stderr or "", "larger than capacity", 1,
                    true) then
  fail("expected oversized query_source chunk error")
end

local projected
projected, err = client:project_file('/status="open"', input_path,
                                    {"/id", "/count"})
projected = assert_no_error(projected, err, "project_file")
assert_equal(projected, '{"id":"b","count":2}\n', "project_file output")

projected, err = client:project_json('/status="open"',
                                    '{"status":"open","id":"c","count":3}',
                                    {"/id", "/count"})
projected = assert_no_error(projected, err, "project_json")
assert_equal(projected, '{"id":"c","count":3}\n', "project_json output")

local mutated
mutated, err = client:mutate_file('/status="open"', input_path,
                                 {"/state/status=running", "rm:/state/old"},
                                 {matches_only = true})
mutated = assert_no_error(mutated, err, "mutate_file")
assert_equal(mutated,
             '{"status":"open","id":"b","count":2,"state":{"status":"running"}}\n',
             "mutate_file output")

mutated, err = client:mutate_json('/status="open"',
                                 '{"status":"open","state":{"old":true}}',
                                 {"/state/status=running", "rm:/state/old"},
                                 {matches_only = true})
mutated = assert_no_error(mutated, err, "mutate_json")
assert_equal(mutated, '{"status":"open","state":{"status":"running"}}\n',
             "mutate_json output")

source_chunks = {
  '{"status":"closed","id":"m1"}\n',
  '{"status":"open","id":"m2","state":{"old":true}}\n'
}
source_index = 1
mutated, err = client:mutate_source('/status="open"', function(_)
  local chunk = source_chunks[source_index]
  source_index = source_index + 1
  return chunk
end, {"/state/status=running", "rm:/state/old"}, {matches_only = true})
mutated = assert_no_error(mutated, err, "mutate_source")
assert_equal(mutated,
             '{"status":"open","id":"m2","state":{"status":"running"}}\n',
             "mutate_source output")

local bad_mutate_source_result, bad_mutate_source_error =
  client:mutate_source('/status="open"', function(_)
    error("mutation source read failed")
  end, {"/state/status=running"}, {matches_only = true})
if bad_mutate_source_result ~= nil or not bad_mutate_source_error or
    not string.find(bad_mutate_source_error.stderr or "",
                    "mutation source read failed", 1, true) then
  fail("expected structured mutate_source read callback error")
end

mutated, err = client:mutate_json('/status="open"', '{"status":"open"}',
                                 {
                                   "textfile:/payload=" .. text_payload_name,
                                   "base64file:/encoded=" .. bin_payload_name
                                 },
                                 {
                                   matches_only = true,
                                   enable_file_mutations = true,
                                   file_value_base_dir = tmp_dir
                                 })
mutated = assert_no_error(mutated, err, "mutate_json file values")
assert_equal(mutated,
             '{"status":"open","payload":"lua\\n\\"payload\\"","encoded":"AAECYQ=="}\n',
             "mutate_json file values output")

mutated, err = client:mutate_file('/status="open"', input_path,
                                 {"textfile:/payload=" .. text_payload_name},
                                 {
                                   matches_only = true,
                                   enable_file_mutations = true,
                                   file_value_base_dir = tmp_dir
                                 })
mutated = assert_no_error(mutated, err, "mutate_file file values")
assert_equal(mutated,
             '{"status":"open","id":"b","count":2,"state":{"old":true},"payload":"lua\\n\\"payload\\""}\n',
             "mutate_file file values output")

local _
_, err = client:select_file('bad{', input_path)
if not err or err.stderr == "" then
  fail("expected structured error for invalid selector")
end

os.remove(input_path)
os.remove(text_payload_path)
os.remove(bin_payload_path)
