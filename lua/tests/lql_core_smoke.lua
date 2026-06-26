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

local function assert_match(client, selector, json, want, message)
  local matched, err = client:matches_json(selector, json)
  matched = assert_no_error(matched, err, message)
  assert_equal(matched, want, message)
end

if os.getenv("LQL_REQUIRE_CORE") == "1" and not lql.has_core() then
  fail("direct lql.core module was not loaded")
end

local client = lql.new({core = true})

local or_selector, or_selector_err =
  client:selector_parse_or('/status="open",/progress>=50')
or_selector = assert_no_error(or_selector, or_selector_err,
                              "core selector_parse_or")

local or_capabilities
or_capabilities, err = client:selector_capabilities(or_selector)
or_capabilities = assert_no_error(or_capabilities, err,
                                  "core selector_parse_or capabilities")
assert_equal(or_capabilities["or"], true,
             "core selector_parse_or capabilities or")
assert_equal(or_capabilities.eq, true, "core selector_parse_or capabilities eq")
assert_equal(or_capabilities.range, true,
             "core selector_parse_or capabilities range")

local bad_or_selector, bad_or_selector_err =
  client:selector_parse_or('eq{field=/status,value=open},nonsense')
if bad_or_selector ~= nil or not bad_or_selector_err or
    (bad_or_selector_err.stderr or "") == "" then
  fail("expected structured core selector_parse_or error")
end

local matched, err = client:matches_json('/status="open"', '{"status":"open"}')
matched = assert_no_error(matched, err, "core matches_json open")
assert_equal(matched, true, "core matches_json open")

matched, err = client:matches_json('/status="open"', '{"status":"closed"}')
matched = assert_no_error(matched, err, "core matches_json closed")
assert_equal(matched, false, "core matches_json closed")

matched, err = client:matches_json(or_selector,
                                  '{"status":"closed","progress":72}')
matched = assert_no_error(matched, err, "core matches_json parsed OR range")
assert_equal(matched, true, "core matches_json parsed OR range")

matched, err = client:matches_json(or_selector,
                                  '{"status":"closed","progress":10}')
matched = assert_no_error(matched, err, "core matches_json parsed OR miss")
assert_equal(matched, false, "core matches_json parsed OR miss")

local compacted
compacted, err =
  client:compact_json(' { "status" : "open", "items" : [ 1, 2 ] } ')
compacted = assert_no_error(compacted, err, "core compact_json")
assert_equal(compacted, '{"status":"open","items":[1,2]}',
             "core compact_json output")

local compact_chunks = {' { "status" : ', '"open", "items" : [ 1, 2 ] } '}
local compact_index = 1
compacted, err = client:compact_source(function(_)
  local chunk = compact_chunks[compact_index]
  compact_index = compact_index + 1
  return chunk
end)
compacted = assert_no_error(compacted, err, "core compact_source")
assert_equal(compacted, '{"status":"open","items":[1,2]}',
             "core compact_source output")

local object_doc =
  '{"hello":{"world":{"nested":true}},"arrays":[{"id":1}]}'
local array_doc = '{"hello":{"world":[1,2,3]},"arrays":[{"id":1}]}'
local null_doc = '{"hello":{"world":null},"arrays":[{"id":1}]}'
local missing_doc = '{"hello":{"other":"x"},"arrays":[{"id":1}]}'
local path_selectors = {
  'contains{f=/hello/world}',
  'icontains{f=/hello/world}',
  'prefix{f=/hello/world}',
  'iprefix{f=/hello/world}'
}

for _, selector in ipairs(path_selectors) do
  assert_match(client, selector, object_doc, true,
               "core omitted string selector object")
  assert_match(client, selector, array_doc, true,
               "core omitted string selector array")
  assert_match(client, selector, null_doc, true,
               "core omitted string selector null")
  assert_match(client, selector, missing_doc, false,
               "core omitted string selector missing")
end

local wildcard_doc =
  '{"hello":{"world":{"nested":true},"names":["alice","bob"]},' ..
  '"arrays":[{"id":1},{"id":2}]}'
assert_match(client, 'contains{f=/hello/*}', wildcard_doc, true,
             "core omitted string object wildcard")
assert_match(client, 'contains{f=/hello/...}', wildcard_doc, true,
             "core omitted string recursive wildcard")
assert_match(client, 'contains{f=/arrays/[]}', wildcard_doc, true,
             "core omitted string array wildcard")
assert_match(client, 'contains{f=/arrays/[]/id}', wildcard_doc, true,
             "core omitted string array child wildcard")
assert_match(client, 'contains{f=/missing/...}', wildcard_doc, false,
             "core omitted string missing recursive wildcard")
assert_match(client, 'not.icontains{f=/,v=""}', '{"status":"open"}', false,
             "core empty root string negation")

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

local projection, projection_err = client:projection_parse({"/id", "/count"})
projection = assert_no_error(projection, projection_err,
                             "core projection_parse")

projected, err = client:project_json('/status="open"',
                                    '{"status":"open","id":"bp","count":12}',
                                    projection)
projected = assert_no_error(projected, err,
                            "core project_json parsed projection")
assert_equal(projected, '{"id":"bp","count":12}\n',
             "core project_json parsed projection output")

local bad_projection_handle, bad_projection_handle_err =
  client:projection_parse({"  ", "\t"})
if bad_projection_handle ~= nil or not bad_projection_handle_err or
    (bad_projection_handle_err.stderr or "") == "" then
  fail("expected structured core projection_parse error")
end

projected, err = client:project_json('/status="open"',
                                    '{"status":"open","id":"trimmed",' ..
                                      '"meta":{"trace":7,"drop":true}}',
                                    {" /id ", "", "/meta/trace", "/id"})
projected = assert_no_error(projected, err,
                            "core project_json normalized fields")
assert_equal(projected, '{"id":"trimmed","meta":{"trace":7}}\n',
             "core project_json normalized fields output")

local blank_projection, blank_projection_err =
  client:project_json('/status="open"', '{"status":"open","id":"x"}',
                      {"  ", "\t"})
if blank_projection ~= nil or not blank_projection_err or
    (blank_projection_err.stderr or "") == "" then
  fail("expected structured core blank projection field error")
end

local nonmatch_blank_projection, nonmatch_blank_projection_err =
  client:project_json('/status="closed"', '{"status":"open","id":"x"}',
                      {"  ", "\t"})
if nonmatch_blank_projection ~= nil or not nonmatch_blank_projection_err or
    (nonmatch_blank_projection_err.stderr or "") == "" then
  fail("expected structured core nonmatch blank projection field error")
end

local mutated
mutated, err = client:mutate_json('/status="open"',
                                 '{"status":"open","state":{"old":true}}',
                                 {"/state/status=running", "rm:/state/old"},
                                 {matches_only = true})
mutated = assert_no_error(mutated, err, "core mutate_json")
assert_equal(mutated, '{"status":"open","state":{"status":"running"}}\n',
             "core mutate_json output")

local mutation_plan, mutation_plan_err =
  client:mutation_plan_parse({"/state/status=planned", "rm:/state/old"})
mutation_plan = assert_no_error(mutation_plan, mutation_plan_err,
                                "core mutation_plan_parse")

local mutation_count, mutation_count_err =
  client:mutation_plan_count(mutation_plan)
mutation_count = assert_no_error(mutation_count, mutation_count_err,
                                 "core mutation_plan_count")
assert_equal(mutation_count, 2, "core mutation_plan_count parsed plan")

mutated, err = client:mutate_json('/status="open"',
                                 '{"status":"open","state":{"old":true}}',
                                 mutation_plan,
                                 {matches_only = true})
mutated = assert_no_error(mutated, err,
                          "core mutate_json parsed mutation plan")
assert_equal(mutated, '{"status":"open","state":{"status":"planned"}}\n',
             "core mutate_json parsed mutation plan output")

local bad_mutation_plan, bad_mutation_plan_err =
  client:mutation_plan_parse({"badexpr"})
if bad_mutation_plan ~= nil or not bad_mutation_plan_err or
    (bad_mutation_plan_err.stderr or "") == "" then
  fail("expected structured core mutation_plan_parse error")
end

mutated, err = client:mutate_json('/status="open"',
                                 '{"status":"closed","state":{"old":true}}',
                                 {"/state/status=running"},
                                 {matches_only = true})
mutated = assert_no_error(mutated, err, "core mutate_json miss")
assert_equal(mutated, "", "core mutate_json miss output")

mutated, err = client:mutate_json('',
                                 '{"items":[{"status":"old"}],' ..
                                   '"boxes":{"a":{"status":"old"}},' ..
                                   '"groups":[{"items":[{"sku":"A",' ..
                                   '"count":1,"drop":true}]}]}',
                                 {"/items/**/status=ready",
                                  "/boxes/**/status=ready",
                                  "/groups/.../sku=Z",
                                  "/groups/.../count=+2"},
                                 {matches_only = true})
mutated = assert_no_error(mutated, err,
                          "core recursive wildcard mutate_json")
assert_equal(mutated,
             '{"items":[{"status":"ready"}],"boxes":{"a":' ..
               '{"status":"ready"}},"groups":[{"items":[{"sku":"Z",' ..
               '"count":3,"drop":true}]}]}\n',
             "core recursive wildcard mutate_json output")

mutated, err = client:mutate_json('',
                                 '{"nums":[1,2],"words":["a","b"],' ..
                                   '"drops":[true,false],' ..
                                   '"objects":[{"a":1}],' ..
                                   '"groups":[{"items":[{"count":1}]}]}',
                                 {"/nums[]=+2",
                                  "/words[]=ready",
                                  "rm:/drops[]",
                                  "/objects[]=done",
                                  "/groups/.../count=+2"},
                                 {matches_only = true})
mutated = assert_no_error(mutated, err,
                          "core array wildcard value mutate_json")
assert_equal(mutated,
             '{"nums":[3,4],"words":["ready","ready"],' ..
               '"drops":[null,null],"objects":["done"],' ..
               '"groups":[{"items":[{"count":3}]}]}\n',
             "core array wildcard value mutate_json output")

local _
_, err = client:matches_json('bad{', '{"status":"open"}')
if not err or err.stderr == "" or not err.status then
  fail("expected structured core error for invalid selector")
end
