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
