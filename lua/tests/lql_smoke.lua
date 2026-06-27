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

local function assert_len(values, want, message)
  if #values ~= want then
    fail(message .. ": got len " .. tostring(#values) ..
         " want " .. tostring(want))
  end
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
local compact_input_path = tmp_base .. ".compact.json"
local limit_input_path = tmp_base .. ".limit.jsonl"
local tmp_dir, tmp_name = string.match(tmp_base, "^(.*)/(.*)$")
if not tmp_dir then
  tmp_dir = "."
  tmp_name = tmp_base
end
local text_payload_name = tmp_name .. ".txt"
local bin_payload_name = tmp_name .. ".bin"
local invalid_utf8_payload_name = tmp_name .. ".invalid-utf8.txt"
local nul_payload_name = tmp_name .. ".nul.txt"
local text_payload_path = tmp_dir .. "/" .. text_payload_name
local bin_payload_path = tmp_dir .. "/" .. bin_payload_name
local invalid_utf8_payload_path = tmp_dir .. "/" .. invalid_utf8_payload_name
local nul_payload_path = tmp_dir .. "/" .. nul_payload_name

write_file(input_path,
           '{"status":"closed","id":"a","count":1}\n' ..
             '{"status":"open","id":"b","count":2,"state":{"old":true}}\n')
write_file(compact_input_path,
           ' { "status" : "open", "items" : [ 1, 2 ] } ')
write_file(limit_input_path,
           '{"status":"open","id":"l1","state":{"old":true}}\n' ..
             '{"status":"open","id":"l2","state":{"old":true}}\n')
write_file(text_payload_path, 'lua\n"payload"')
write_file(bin_payload_path, string.char(0, 1, 2, 97))
write_file(invalid_utf8_payload_path, "lua" .. string.char(255))
write_file(nul_payload_path, "lua" .. string.char(0) .. "payload")

if os.getenv("LQL_REQUIRE_CORE") == "1" and not lql.has_core() then
  fail("direct lql.core module was not loaded")
end

local client = lql.new()

local version = client:version()
if type(version) ~= "string" or version == "" then
  fail("expected non-empty client version")
end

local client_capabilities = client:capabilities()
assert_equal(client_capabilities.selector_parse, true,
             "client capabilities selector_parse")
assert_equal(client_capabilities.selector_inspection, true,
             "client capabilities selector_inspection")
assert_equal(client_capabilities.matches_json, true,
             "client capabilities matches_json")
assert_equal(client_capabilities.file_decision_stream, true,
             "client capabilities file_decision_stream")
assert_equal(client_capabilities.source_decision_stream, true,
             "client capabilities source_decision_stream")
assert_equal(client_capabilities.file_match_stream, true,
             "client capabilities file_match_stream")
assert_equal(client_capabilities.seekable_range_payloads, true,
             "client capabilities seekable_range_payloads")
assert_equal(client_capabilities.source_spooled_match_stream, true,
             "client capabilities source_spooled_match_stream")
assert_equal(client_capabilities.payload_sink_write, true,
             "client capabilities payload_sink_write")
assert_equal(client_capabilities.payload_projection, true,
             "client capabilities payload_projection")
assert_equal(client_capabilities.projection_source, true,
             "client capabilities projection_source")
assert_equal(client_capabilities.compact_source, true,
             "client capabilities compact_source")
assert_equal(client_capabilities.mutation_source, true,
             "client capabilities mutation_source")
assert_equal(client_capabilities.mutation_file_values, true,
             "client capabilities mutation_file_values")

local open_selector, selector_err =
  client:selector_parse('/status="open"')
open_selector = assert_no_error(open_selector, selector_err,
                                "selector_parse open")

local or_selector, or_selector_err =
  client:selector_parse_or('/status="open",/progress>=50')
or_selector = assert_no_error(or_selector, or_selector_err,
                              "selector_parse_or")

local invalid_selector, invalid_selector_err =
  client:selector_parse('eq{field=/status,value=open,foo=bar}')
if invalid_selector ~= nil or not invalid_selector_err or
    invalid_selector_err.status ~= 3 or
    (invalid_selector_err.stderr or "") == "" then
  fail("expected structured selector_parse error")
end

local capabilities
capabilities, err = client:selector_capabilities(
                      'and.eq{field=/status,value=open},' ..
                        'icontains{field=/msg,value=timeout},' ..
                        'exists{/meta/**/etag}')
capabilities = assert_no_error(capabilities, err, "selector_capabilities")
assert_equal(capabilities["and"], true, "selector_capabilities and")
assert_equal(capabilities.eq, true, "selector_capabilities eq")
assert_equal(capabilities.contains, true, "selector_capabilities contains")
assert_equal(capabilities.exists, true, "selector_capabilities exists")
assert_equal(capabilities.wildcard_path, true,
             "selector_capabilities wildcard")
assert_equal(capabilities.recursive_path, true,
             "selector_capabilities recursive")

local parsed_capabilities
parsed_capabilities, err = client:selector_capabilities(open_selector)
parsed_capabilities = assert_no_error(parsed_capabilities, err,
                                      "selector_capabilities parsed")
assert_equal(parsed_capabilities.eq, true, "parsed selector_capabilities eq")
assert_equal(parsed_capabilities.recursive_path, false,
             "parsed selector_capabilities recursive")

local traits
traits, err = client:selector_execution_traits(
                'and.eq{field=/status,value=open},' ..
                  'icontains{field=/msg,value=timeout},' ..
                  'exists{/meta/**/etag}')
traits = assert_no_error(traits, err, "selector_execution_traits")
assert_equal(traits.uses_contains_like, true,
             "selector_execution_traits contains")
assert_equal(traits.uses_recursive_path, true,
             "selector_execution_traits recursive")
assert_equal(traits.uses_wildcard_path, true,
             "selector_execution_traits wildcard")
assert_equal(traits.requires_object_root, true,
             "selector_execution_traits root")
assert_equal(traits.early_non_match_likely, false,
             "selector_execution_traits early")

local match_all_traits
match_all_traits, err =
  client:selector_execution_traits('icontains{f=/,v=""}')
match_all_traits = assert_no_error(match_all_traits, err,
                                   "match-all selector_execution_traits")
assert_equal(match_all_traits.requires_object_root, false,
             "match-all selector_execution_traits root")
assert_equal(match_all_traits.uses_contains_like, false,
             "match-all selector_execution_traits contains")

local matched

local or_root = assert_no_error(client:selector_root(or_selector), nil,
                                "selector_root OR")
assert_equal(or_root.kind, "or", "selector_root OR kind")
assert_len(or_root.children, 2, "selector_root OR children")
assert_equal(or_root.children[1].kind, "eq", "selector_root OR first kind")
assert_equal(or_root.children[1].field, "/status",
             "selector_root OR first field")
assert_equal(or_root.children[1].value, "open",
             "selector_root OR first value")
assert_equal(or_root.children[2].kind, "range",
             "selector_root OR second kind")
assert_equal(or_root.children[2].field, "/progress",
             "selector_root OR second field")
assert_equal(or_root.children[2].gte, 50,
             "selector_root OR second gte")
assert_equal(or_root.children[2].gte_kind, "number",
             "selector_root OR second gte kind")

local method_root = assert_no_error(or_selector:root(), nil,
                                    "selector method root")
assert_equal(method_root.kind, "or", "selector method root kind")

local selector_json = assert_no_error(or_selector:json(), nil,
                                      "selector method json")
assert_equal(selector_json,
             '{"or":[{"eq":{"field":"/status","value":"open"}},' ..
               '{"range":{"field":"/progress","gte":50}}]}',
             "selector method json output")

local json_selector, json_selector_err =
  client:selector_parse_json(selector_json)
json_selector = assert_no_error(json_selector, json_selector_err,
                                "selector_parse_json round trip")
matched, err = client:matches_json(json_selector,
                                  '{"status":"closed","progress":72}')
matched = assert_no_error(matched, err, "selector_parse_json match")
assert_equal(matched, true, "selector_parse_json match output")
assert_equal(client:selector_json(json_selector), selector_json,
             "client selector_json output")
assert_equal(or_selector:is_empty(), false, "selector method is_empty false")
assert_equal(json_selector:capabilities()["or"], true,
             "selector method capabilities")
assert_equal(json_selector:execution_traits().early_non_match_likely, true,
             "selector method execution traits")

local all_selector, all_err = client:selector_all()
all_selector = assert_no_error(all_selector, all_err, "selector_all")
assert_equal(all_selector:is_empty(), true, "selector_all is empty")
assert_equal(all_selector:json(), "{}", "selector_all json")
assert_equal(all_selector:root().kind, "all", "selector_all root")

local eq_selector, eq_err =
  client:selector_string("eq", {field = "/status", value = "open"})
eq_selector = assert_no_error(eq_selector, eq_err, "selector_string eq")
assert_equal(eq_selector:json(),
             '{"eq":{"field":"/status","value":"open"}}',
             "selector_string eq json")
matched, err = client:matches_json(eq_selector, '{"status":"open"}')
matched = assert_no_error(matched, err, "selector_string eq match")
assert_equal(matched, true, "selector_string eq match output")

local contains_selector, contains_err =
  client:selector_string("contains", {field = "/msg", ignore_case = true},
                         {"WARN", "timeout"})
contains_selector = assert_no_error(contains_selector, contains_err,
                                    "selector_string contains any")
local contains_root = contains_selector:root()
assert_equal(contains_root.kind, "contains",
             "selector_string contains root kind")
assert_equal(contains_root.ignore_case, true,
             "selector_string contains ignore_case")
assert_len(contains_root.any, 2, "selector_string contains any len")
matched, err = client:matches_json(contains_selector,
                                  '{"msg":"warn before timeout"}')
matched = assert_no_error(matched, err, "selector_string contains match")
assert_equal(matched, true, "selector_string contains match output")

local range_selector, range_err =
  client:selector_range({field = "/progress", gte = 10})
range_selector = assert_no_error(range_selector, range_err,
                                 "selector_range numeric")
assert_equal(range_selector:root().gte_kind, "number",
             "selector_range numeric kind")

local datetime_range_selector, datetime_range_err =
  client:selector_range({field = "/timestamp",
                         gte = "2026-03-05T10:28:21Z",
                         lt = "2026-03-05T10:30:00Z"})
datetime_range_selector =
  assert_no_error(datetime_range_selector, datetime_range_err,
                  "selector_range datetime")
assert_equal(datetime_range_selector:root().gte_kind, "datetime",
             "selector_range datetime kind")
matched, err = client:matches_json(datetime_range_selector,
                                  '{"timestamp":"2026-03-05T10:29:00Z"}')
matched = assert_no_error(matched, err, "selector_range datetime match")
assert_equal(matched, true, "selector_range datetime match output")

local date_selector, date_err =
  client:selector_date({field = "/timestamp",
                        after = "2025-01-01",
                        before = "2025-01-03"})
date_selector = assert_no_error(date_selector, date_err, "selector_date")
assert_equal(date_selector:root().after, "2025-01-01",
             "selector_date after")
matched, err = client:matches_json(date_selector,
                                  '{"timestamp":"2025-01-02T00:00:00Z"}')
matched = assert_no_error(matched, err, "selector_date match")
assert_equal(matched, true, "selector_date match output")

local since_selector, since_err =
  client:selector_date({field = "/timestamp", since_kind = "today"})
since_selector = assert_no_error(since_selector, since_err,
                                 "selector_date since")
assert_equal(since_selector:root().since_kind, "today",
             "selector_date since kind")

local in_selector, in_err =
  client:selector_in({field = "/env"}, {"prod", "stage"})
in_selector = assert_no_error(in_selector, in_err, "selector_in")
assert_equal(in_selector:root().any[2], "stage", "selector_in any")
matched, err = client:matches_json(in_selector, '{"env":"stage"}')
matched = assert_no_error(matched, err, "selector_in match")
assert_equal(matched, true, "selector_in match output")

local exists_selector, exists_err = client:selector_exists("/meta/etag")
exists_selector = assert_no_error(exists_selector, exists_err,
                                  "selector_exists")
assert_equal(exists_selector:root().path, "/meta/etag",
             "selector_exists path")

local and_selector, and_err =
  client:selector_compound("and", {eq_selector, range_selector})
and_selector = assert_no_error(and_selector, and_err,
                               "selector_compound and")
matched, err = client:matches_json(and_selector,
                                  '{"status":"open","progress":25}')
matched = assert_no_error(matched, err, "selector_compound and match")
assert_equal(matched, true, "selector_compound and match output")

local not_selector, not_err = client:selector_not(eq_selector)
not_selector = assert_no_error(not_selector, not_err, "selector_not")
matched, err = client:matches_json(not_selector, '{"status":"closed"}')
matched = assert_no_error(matched, err, "selector_not match")
assert_equal(matched, true, "selector_not match output")

local bad_json_selector, bad_json_err =
  client:selector_parse_json('{"eq":{"field":"/x","bad":true}}')
if bad_json_selector ~= nil or not bad_json_err or
    bad_json_err.status ~= 3 then
  fail("expected structured selector_parse_json error")
end

local bad_range_selector, bad_range_err =
  client:selector_range({field = "/progress"})
if bad_range_selector ~= nil or not bad_range_err or
    bad_range_err.status ~= 3 then
  fail("expected structured selector_range error")
end

matched, err = client:matches_json('/status="open"', '{"status":"open"}')
matched = assert_no_error(matched, err, "matches_json open")
assert_equal(matched, true, "matches_json open")

matched, err = client:matches_json(open_selector, '{"status":"open"}')
matched = assert_no_error(matched, err, "matches_json parsed selector open")
assert_equal(matched, true, "matches_json parsed selector open")

matched, err = client:matches_json(or_selector,
                                  '{"status":"closed","progress":72}')
matched = assert_no_error(matched, err, "matches_json parsed selector OR")
assert_equal(matched, true, "matches_json parsed selector OR")

matched, err = client:matches_json('/status="open"', '{"status":"closed"}')
matched = assert_no_error(matched, err, "matches_json closed")
assert_equal(matched, false, "matches_json closed")

local compacted
compacted, err =
  client:compact_json(' { "status" : "open", "items" : [ 1, 2 ] } ')
compacted = assert_no_error(compacted, err, "compact_json")
assert_equal(compacted, '{"status":"open","items":[1,2]}',
             "compact_json output")

compacted, err = client:compact_file(compact_input_path)
compacted = assert_no_error(compacted, err, "compact_file")
assert_equal(compacted, '{"status":"open","items":[1,2]}',
             "compact_file output")

local compact_chunks = {' { "status" : ', '"open", "items" : [ 1, 2 ] } '}
local compact_index = 1
compacted, err = client:compact_source(function(_)
  local chunk = compact_chunks[compact_index]
  compact_index = compact_index + 1
  return chunk
end)
compacted = assert_no_error(compacted, err, "compact_source")
assert_equal(compacted, '{"status":"open","items":[1,2]}',
             "compact_source output")

local bad_compact_source_result, bad_compact_source_error =
  client:compact_source(function(_)
    error("compact source read failed")
  end)
if bad_compact_source_result ~= nil or not bad_compact_source_error or
    not string.find(bad_compact_source_error.stderr or "",
                    "compact source read failed", 1, true) then
  fail("expected structured compact_source read callback error")
end

local selected
selected, err = client:select_file('/status="open"', input_path,
                                  {compact = true})
selected = assert_no_error(selected, err, "select_file")
assert_equal(selected,
             '{"status":"open","id":"b","count":2,"state":{"old":true}}\n',
             "select_file output")

selected, err = client:select_file(open_selector, input_path, {compact = true})
selected = assert_no_error(selected, err, "select_file parsed selector")
assert_equal(selected,
             '{"status":"open","id":"b","count":2,"state":{"old":true}}\n',
             "select_file parsed selector output")

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

local parsed_decisions = {}
result, err = client:query_file(open_selector, input_path, function(decision)
  parsed_decisions[#parsed_decisions + 1] = decision
end)
result = assert_no_error(result, err, "query_file parsed selector")
assert_equal(result.candidates_seen, 2, "query_file parsed selector candidates")
assert_equal(result.candidates_matched, 1, "query_file parsed selector matches")
assert_equal(parsed_decisions[2].matched, true,
             "query_file parsed selector second match")

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

source_chunks = {'[{"id":"a"},[{"id":"b"}],{"id":"c"}]'}
source_index = 1
source_decisions = {}
result, err = client:query_source('/id="b"', function(capacity)
  local chunk = source_chunks[source_index]
  if not chunk then
    return nil
  end
  if #chunk > capacity then
    source_chunks[source_index] = string.sub(chunk, capacity + 1)
    return string.sub(chunk, 1, capacity)
  end
  source_index = source_index + 1
  return chunk
end, function(decision)
  source_decisions[#source_decisions + 1] = decision
end)
result = assert_no_error(result, err, "query_source nested array")
assert_equal(result.candidates_seen, 3,
             "query_source nested array candidates")
assert_equal(result.candidates_matched, 1,
             "query_source nested array matches")
assert_equal(source_decisions[1].matched, false,
             "query_source nested array first miss")
assert_equal(source_decisions[2].matched, true,
             "query_source nested array second match")
assert_equal(source_decisions[3].matched, false,
             "query_source nested array third miss")

source_chunks = {
  '{"status":"closed","id":"ss1"}\n',
  '{"status":"open","id":"ss2","count":6}\n'
}
source_index = 1
selected, err = client:select_source('/status="open"', function(_)
  local chunk = source_chunks[source_index]
  source_index = source_index + 1
  return chunk
end)
selected = assert_no_error(selected, err, "select_source")
assert_equal(selected, '{"status":"open","id":"ss2","count":6}\n',
             "select_source output")

source_chunks = {
  '{"status":"closed","id":"ssp1"}\n',
  '{"status":"open","id":"ssp2","count":8}\n'
}
source_index = 1
selected, err = client:select_source(open_selector, function(_)
  local chunk = source_chunks[source_index]
  source_index = source_index + 1
  return chunk
end)
selected = assert_no_error(selected, err, "select_source parsed selector")
assert_equal(selected, '{"status":"open","id":"ssp2","count":8}\n',
             "select_source parsed selector output")

local bad_select_source_result, bad_select_source_error =
  client:select_source('/status="open"', function(_)
    error("selection source read failed")
  end)
if bad_select_source_result ~= nil or not bad_select_source_error or
    not string.find(bad_select_source_error.stderr or "",
                    "selection source read failed", 1, true) then
  fail("expected structured select_source read callback error")
end

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

local parsed_payload
result, err = client:each_match_file(open_selector, input_path, function(match)
  parsed_payload = assert_no_error(match.json(), nil,
                                   "each_match_file parsed selector payload")
end)
result = assert_no_error(result, err, "each_match_file parsed selector")
assert_equal(result.candidates_matched, 1,
             "each_match_file parsed selector matches")
assert_equal(parsed_payload,
             '{"status":"open","id":"b","count":2,"state":{"old":true}}',
             "each_match_file parsed selector payload")

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

source_chunks = {
  '{"status":"closed","id":"pp1"}\n',
  '{"status":"open","id":"pp2","count":9}\n'
}
source_index = 1
local parsed_source_payload
result, err = client:each_match_source(open_selector, function(_)
  local chunk = source_chunks[source_index]
  source_index = source_index + 1
  return chunk
end, function(match)
  parsed_source_payload = assert_no_error(match.json(), nil,
                                          "each_match_source parsed payload")
end)
result = assert_no_error(result, err, "each_match_source parsed selector")
assert_equal(result.candidates_matched, 1,
             "each_match_source parsed selector matches")
assert_equal(parsed_source_payload, '{"status":"open","id":"pp2","count":9}',
             "each_match_source parsed selector payload")

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

local function expect_oversized_source_error(label, fn)
  local value, source_error = fn(function(_)
    return string.rep("x", 9000)
  end)
  if value ~= nil or not source_error or
      not string.find(source_error.stderr or "", "larger than capacity", 1,
                      true) then
    fail("expected oversized " .. label .. " chunk error")
  end
end

expect_oversized_source_error("select_source", function(read_fn)
  return client:select_source('/status="open"', read_fn)
end)

expect_oversized_source_error("each_match_source", function(read_fn)
  return client:each_match_source('/status="open"', read_fn, function(_) end)
end)

local projected
projected, err = client:project_file('/status="open"', input_path,
                                    {"/id", "/count"})
projected = assert_no_error(projected, err, "project_file")
assert_equal(projected, '{"id":"b","count":2}\n', "project_file output")

local projection, projection_err = client:projection_parse({"/id", "/count"})
projection = assert_no_error(projection, projection_err, "projection_parse")

projected, err = client:project_file('/status="open"', input_path, projection)
projected = assert_no_error(projected, err, "project_file parsed projection")
assert_equal(projected, '{"id":"b","count":2}\n',
             "project_file parsed projection output")

projected, err = client:project_file(open_selector, input_path,
                                     {"/id", "/count"})
projected = assert_no_error(projected, err, "project_file parsed selector")
assert_equal(projected, '{"id":"b","count":2}\n',
             "project_file parsed selector output")

projected, err = client:project_json('/status="open"',
                                    '{"status":"open","id":"c","count":3}',
                                    {"/id", "/count"})
projected = assert_no_error(projected, err, "project_json")
assert_equal(projected, '{"id":"c","count":3}\n', "project_json output")

projected, err = client:project_json('/status="open"',
                                    '{"status":"open","id":"ch","count":13}',
                                    projection)
projected = assert_no_error(projected, err, "project_json parsed projection")
assert_equal(projected, '{"id":"ch","count":13}\n',
             "project_json parsed projection output")

projected, err = client:project_json(open_selector,
                                     '{"status":"open","id":"cp","count":4}',
                                     {"/id", "/count"})
projected = assert_no_error(projected, err, "project_json parsed selector")
assert_equal(projected, '{"id":"cp","count":4}\n',
             "project_json parsed selector output")

source_chunks = {
  '{"status":"closed","id":"ps1","count":1}\n',
  '{"status":"open","id":"ps2","count":7}\n'
}
source_index = 1
projected, err = client:project_source('/status="open"', function(_)
  local chunk = source_chunks[source_index]
  source_index = source_index + 1
  return chunk
end, {"/id", "/count"})
projected = assert_no_error(projected, err, "project_source")
assert_equal(projected, '{"id":"ps2","count":7}\n', "project_source output")

source_chunks = {
  '{"status":"closed","id":"psh1","count":1}\n',
  '{"status":"open","id":"psh2","count":15}\n'
}
source_index = 1
projected, err = client:project_source('/status="open"', function(_)
  local chunk = source_chunks[source_index]
  source_index = source_index + 1
  return chunk
end, projection)
projected = assert_no_error(projected, err,
                            "project_source parsed projection")
assert_equal(projected, '{"id":"psh2","count":15}\n',
             "project_source parsed projection output")

source_chunks = {
  '{"status":"closed","id":"psp1","count":1}\n',
  '{"status":"open","id":"psp2","count":11}\n'
}
source_index = 1
projected, err = client:project_source(open_selector, function(_)
  local chunk = source_chunks[source_index]
  source_index = source_index + 1
  return chunk
end, {"/id", "/count"})
projected = assert_no_error(projected, err, "project_source parsed selector")
assert_equal(projected, '{"id":"psp2","count":11}\n',
             "project_source parsed selector output")

expect_oversized_source_error("project_source", function(read_fn)
  return client:project_source('/status="open"', read_fn, {"/id"})
end)

local bad_project_source_result, bad_project_source_error =
  client:project_source('/status="open"', function(_)
    error("projection source read failed")
  end, {"/id"})
if bad_project_source_result ~= nil or not bad_project_source_error or
    not string.find(bad_project_source_error.stderr or "",
                    "projection source read failed", 1, true) then
  fail("expected structured project_source read callback error")
end

local bad_projection, bad_projection_err = client:projection_parse({"  "})
if bad_projection ~= nil or not bad_projection_err or
    (bad_projection_err.stderr or "") == "" then
  fail("expected structured projection_parse error")
end

local mutated
mutated, err = client:mutate_file('/status="open"', input_path,
                                 {"/state/status=running", "rm:/state/old"},
                                 {matches_only = true})
mutated = assert_no_error(mutated, err, "mutate_file")
assert_equal(mutated,
             '{"status":"open","id":"b","count":2,"state":{"status":"running"}}\n',
             "mutate_file output")

local mutation_plan, mutation_plan_err =
  client:mutation_plan_parse({"/state/status=planned", "rm:/state/old"})
mutation_plan = assert_no_error(mutation_plan, mutation_plan_err,
                                "mutation_plan_parse")

local mutation_plan_count, mutation_plan_count_err =
  client:mutation_plan_count(mutation_plan)
mutation_plan_count = assert_no_error(mutation_plan_count,
                                      mutation_plan_count_err,
                                      "mutation_plan_count")
assert_equal(mutation_plan_count, 2, "mutation_plan_count parsed plan")

mutated, err = client:mutate_file('/status="open"', input_path,
                                 mutation_plan,
                                 {matches_only = true})
mutated = assert_no_error(mutated, err, "mutate_file parsed mutation plan")
assert_equal(mutated,
             '{"status":"open","id":"b","count":2,"state":{"status":"planned"}}\n',
             "mutate_file parsed mutation plan output")

mutated, err = client:mutate_file('/status="open"', limit_input_path,
                                 {"/state/status=limited", "rm:/state/old"},
                                 {
                                   matches_only = true,
                                   max_matches = 1
                                 })
mutated = assert_no_error(mutated, err, "mutate_file max_matches")
assert_equal(mutated,
             '{"status":"open","id":"l1","state":{"status":"limited"}}\n',
             "mutate_file max_matches output")

mutated, err = client:mutate_file(open_selector, input_path,
                                 {"/state/status=parsed", "rm:/state/old"},
                                 {matches_only = true})
mutated = assert_no_error(mutated, err, "mutate_file parsed selector")
assert_equal(mutated,
             '{"status":"open","id":"b","count":2,"state":{"status":"parsed"}}\n',
             "mutate_file parsed selector output")

mutated, err = client:mutate_json('/status="open"',
                                 '{"status":"open","state":{"old":true}}',
                                 {"/state/status=running", "rm:/state/old"},
                                 {matches_only = true})
mutated = assert_no_error(mutated, err, "mutate_json")
assert_equal(mutated, '{"status":"open","state":{"status":"running"}}\n',
             "mutate_json output")

mutated, err = client:mutate_json('/status="open"',
                                 '{"status":"open","state":{"old":true}}',
                                 mutation_plan,
                                 {matches_only = true})
mutated = assert_no_error(mutated, err, "mutate_json parsed mutation plan")
assert_equal(mutated, '{"status":"open","state":{"status":"planned"}}\n',
             "mutate_json parsed mutation plan output")

mutated, err = client:mutate_json(open_selector,
                                 '{"status":"open","state":{"old":true}}',
                                 {"/state/status=parsed", "rm:/state/old"},
                                 {matches_only = true})
mutated = assert_no_error(mutated, err, "mutate_json parsed selector")
assert_equal(mutated, '{"status":"open","state":{"status":"parsed"}}\n',
             "mutate_json parsed selector output")

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

source_chunks = {
  '{"status":"closed","id":"mh1"}\n',
  '{"status":"open","id":"mh2","state":{"old":true}}\n'
}
source_index = 1
mutated, err = client:mutate_source('/status="open"', function(_)
  local chunk = source_chunks[source_index]
  source_index = source_index + 1
  return chunk
end, mutation_plan, {matches_only = true})
mutated = assert_no_error(mutated, err, "mutate_source parsed mutation plan")
assert_equal(mutated,
             '{"status":"open","id":"mh2","state":{"status":"planned"}}\n',
             "mutate_source parsed mutation plan output")

source_chunks = {
  '{"status":"open","id":"ml1","state":{"old":true}}\n',
  '{"status":"open","id":"ml2","state":{"old":true}}\n'
}
source_index = 1
mutated, err = client:mutate_source('/status="open"', function(_)
  local chunk = source_chunks[source_index]
  source_index = source_index + 1
  return chunk
end, {"/state/status=limited", "rm:/state/old"},
  {
    matches_only = true,
    max_matches = 1
  })
mutated = assert_no_error(mutated, err, "mutate_source max_matches")
assert_equal(mutated,
             '{"status":"open","id":"ml1","state":{"status":"limited"}}\n',
             "mutate_source max_matches output")

source_chunks = {
  '{"event":"tabs_update","component":"host","id":1}\n',
  '{"event":"noop","component":"host","id":2}\n',
  '{"event":"tabs_update","component":"host","id":3}\n',
  '{"event":"tabs_update","component":"host","id":4}'
}
source_index = 1
mutated, err = client:mutate_source('/event="tabs_update"', function(_)
  local chunk = source_chunks[source_index]
  source_index = source_index + 1
  return chunk
end, {
  "/processed=true",
  "time:/processed_at=2023-11-14T22:13:20Z"
}, {matches_only = true})
mutated = assert_no_error(mutated, err, "mutate_source query-mutate handoff")
assert_equal(mutated,
             '{"event":"tabs_update","component":"host","id":1,"processed":true,"processed_at":"2023-11-14T22:13:20Z"}\n' ..
             '{"event":"tabs_update","component":"host","id":3,"processed":true,"processed_at":"2023-11-14T22:13:20Z"}\n' ..
             '{"event":"tabs_update","component":"host","id":4,"processed":true,"processed_at":"2023-11-14T22:13:20Z"}\n',
             "mutate_source query-mutate handoff output")

source_chunks = {
  '{"status":"closed","id":"mp1"}\n',
  '{"status":"open","id":"mp2","state":{"old":true}}\n'
}
source_index = 1
mutated, err = client:mutate_source(open_selector, function(_)
  local chunk = source_chunks[source_index]
  source_index = source_index + 1
  return chunk
end, {"/state/status=parsed", "rm:/state/old"}, {matches_only = true})
mutated = assert_no_error(mutated, err, "mutate_source parsed selector")
assert_equal(mutated,
             '{"status":"open","id":"mp2","state":{"status":"parsed"}}\n',
             "mutate_source parsed selector output")

local bad_mutate_source_result, bad_mutate_source_error =
  client:mutate_source('/status="open"', function(_)
    error("mutation source read failed")
  end, {"/state/status=running"}, {matches_only = true})
if bad_mutate_source_result ~= nil or not bad_mutate_source_error or
    not string.find(bad_mutate_source_error.stderr or "",
                    "mutation source read failed", 1, true) then
  fail("expected structured mutate_source read callback error")
end

local bad_mutation_plan, bad_mutation_plan_err =
  client:mutation_plan_parse({"badexpr"})
if bad_mutation_plan ~= nil or not bad_mutation_plan_err or
    (bad_mutation_plan_err.stderr or "") == "" then
  fail("expected structured mutation_plan_parse error")
end

expect_oversized_source_error("mutate_source", function(read_fn)
  return client:mutate_source('/status="open"', read_fn,
                              {"/state/status=running"},
                              {matches_only = true})
end)

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

source_chunks = {
  '{"status":"closed","id":"mf1"}\n',
  '{"status":"open","id":"mf2"}\n'
}
source_index = 1
mutated, err = client:mutate_source('/status="open"', function(_)
  local chunk = source_chunks[source_index]
  source_index = source_index + 1
  return chunk
end, {"textfile:/payload=" .. text_payload_name},
  {
    matches_only = true,
    enable_file_mutations = true,
    file_value_base_dir = tmp_dir
  })
mutated = assert_no_error(mutated, err, "mutate_source file values")
assert_equal(mutated,
             '{"status":"open","id":"mf2","payload":"lua\\n\\"payload\\""}\n',
             "mutate_source file values output")

local invalid_text_result, invalid_text_error =
  client:mutate_json('/status="open"', '{"status":"open"}',
                     {"textfile:/payload=" .. invalid_utf8_payload_name},
                     {
                       matches_only = true,
                       enable_file_mutations = true,
                       file_value_base_dir = tmp_dir
                     })
if invalid_text_result ~= nil or not invalid_text_error or
    not string.find(invalid_text_error.stderr or "", "UTF", 1, true) then
  fail("expected structured Lua invalid UTF-8 textfile mutation error")
end

local nul_text_result, nul_text_error =
  client:mutate_json('/status="open"', '{"status":"open"}',
                     {"textfile:/payload=" .. nul_payload_name},
                     {
                       matches_only = true,
                       enable_file_mutations = true,
                       file_value_base_dir = tmp_dir
                     })
if nul_text_result ~= nil or not nul_text_error or
    not string.find(nul_text_error.stderr or "", "NUL", 1, true) then
  fail("expected structured Lua NUL textfile mutation error")
end

local _
_, err = client:select_file('bad{', input_path)
if not err or err.stderr == "" then
  fail("expected structured error for invalid selector")
end

os.remove(input_path)
os.remove(compact_input_path)
os.remove(text_payload_path)
os.remove(bin_payload_path)
os.remove(invalid_utf8_payload_path)
os.remove(nul_payload_path)
