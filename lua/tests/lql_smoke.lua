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

assert_equal(lql.has_core(), true, "core module loaded")

local client = assert_no_error(lql.new(), nil, "new client")
assert_truthy(type(client:version()) == "string", "client version")

local caps = client:capabilities()
assert_equal(caps.selector_parse, true, "selector_parse capability")
assert_equal(caps.selector_inspection, true, "selector_inspection capability")
assert_equal(caps.execute_string, true, "execute_string capability")

local selector = assert_no_error(client:selector_parse('/status="open"'), nil,
                                 "selector parse")
assert_equal(selector:is_empty(), false, "non-empty selector")
assert_equal(selector:capabilities().eq, true, "selector eq capability")
assert_equal(client:selector_capabilities(selector).eq, true,
             "client selector capability from userdata")
assert_equal(client:selector_capabilities('/status="open"').eq, true,
             "client selector capability from string")

local result = assert_no_error(
  client:execute_string(selector,
                        '{"status":"closed","id":1}\n' ..
                        '{"status":"open","id":2}\n'),
  nil,
  "execute string")
assert_equal(result.records_seen, 2, "records seen")
assert_equal(result.records_matched, 1, "records matched")
assert_equal(result.output, '{"status":"open","id":2}\n', "selected output")

local count = assert_no_error(
  client:execute_string('/status="open"',
                        '{"status":"open"}\n{"status":"closed"}\n',
                        {count = true}),
  nil,
  "execute string count")
assert_equal(count.records_seen, 2, "count records seen")
assert_equal(count.records_matched, 1, "count records matched")
assert_equal(count.output, nil, "count has no output")

local invalid_selector, invalid_err = client:selector_parse('eq{bad')
if invalid_selector ~= nil or not invalid_err or invalid_err.status == 0 then
  fail("expected structured selector parse error")
end

local invalid_run, invalid_run_err =
  client:execute_string(selector, '{"status":"open"}\n[')
if invalid_run ~= nil or not invalid_run_err or invalid_run_err.status == 0 then
  fail("expected structured execute error")
end
