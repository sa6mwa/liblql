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
    fail(message .. ": " .. (err.stderr or tostring(err.status)))
  end
  return value
end

if os.getenv("LQL_REQUIRE_CORE") == "1" and not lql.has_core() then
  fail("direct lql.core module was not loaded")
end

local client = lql.new({core = true})

local matched, err = client:matches_json('/status="open"', '{"status":"open"}')
matched = assert_no_error(matched, err, "core matches_json open")
assert_equal(matched, true, "core matches_json open")

matched, err = client:matches_json('/status="open"', '{"status":"closed"}')
matched = assert_no_error(matched, err, "core matches_json closed")
assert_equal(matched, false, "core matches_json closed")

local selected
selected, err = client:select_json('/status="open"',
                                  '{"status":"open","id":"a"}',
                                  {compact = true})
selected = assert_no_error(selected, err, "core select_json")
assert_equal(selected, '{"status":"open","id":"a"}\n',
             "core select_json output")

selected, err = client:select_json('/status="open"', '{"status":"closed"}',
                                  {compact = true})
selected = assert_no_error(selected, err, "core select_json miss")
assert_equal(selected, "", "core select_json miss output")

local projected
projected, err = client:project_json('/status="open"',
                                    '{"status":"open","id":"b","count":2}',
                                    {"/id", "/count"})
projected = assert_no_error(projected, err, "core project_json")
assert_equal(projected, '{"id":"b","count":2}\n',
             "core project_json output")

local mutated
mutated, err = client:mutate_json('/status="open"',
                                 '{"status":"open","state":{"old":true}}',
                                 {"/state/status=running", "rm:/state/old"},
                                 {matches_only = true})
mutated = assert_no_error(mutated, err, "core mutate_json")
assert_equal(mutated, '{"status":"open","state":{"status":"running"}}\n',
             "core mutate_json output")

mutated, err = client:mutate_json('/status="open"',
                                 '{"status":"closed","state":{"old":true}}',
                                 {"/state/status=running"},
                                 {matches_only = true})
mutated = assert_no_error(mutated, err, "core mutate_json miss")
assert_equal(mutated, "", "core mutate_json miss output")

local _
_, err = client:matches_json('bad{', '{"status":"open"}')
if not err or err.stderr == "" or not err.status then
  fail("expected structured core error for invalid selector")
end
