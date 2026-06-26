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

write_file(input_path,
           '{"status":"closed","id":"a","count":1}\n' ..
             '{"status":"open","id":"b","count":2,"state":{"old":true}}\n')

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

local _
_, err = client:select_file('bad{', input_path)
if not err or err.stderr == "" then
  fail("expected structured error for invalid selector")
end

os.remove(input_path)
