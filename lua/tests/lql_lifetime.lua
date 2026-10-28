local core = require("lql.core")
local function finalize(handle)
  assert(handle:close())
end
local function failed(value, err)
  assert(value == nil and err and err.status_string == "callback error",
         err and err.message or "expected callback error")
end
local function expired(value, method, ...)
  assert(not pcall(value[method], value, ...), "finalized view remained usable")
end
local function methods(handle, expected)
  if getmetatable(handle).__gc then
    expected[#expected + 1] = "close"
    assert(type(getmetatable(handle).__close) == "function")
  else
    assert(getmetatable(handle).__close == nil)
  end
  local actual = {}
  for name in pairs(getmetatable(handle).__index) do actual[#actual + 1] = name end
  table.sort(actual)
  table.sort(expected)
  assert(table.concat(actual, ",") == table.concat(expected, ","), "facade surface changed")
end
local c = assert(core.new())
methods(c, {"version", "capabilities", "selector_parse", "selector_parse_or",
  "selector_parse_json", "selector_build_all", "selector_build_compound",
  "selector_build_not", "selector_build_string", "selector_build_range",
  "selector_build_date", "selector_build_in", "selector_build_exists",
  "path_is_regular_file", "projection_parse", "mutation_parse", "selector_capabilities",
  "stream_apply", "stream_apply_spooled", "apply_string_spooled",
  "filter_file_spooled", "rewrite_file_inline_spooled"})
local s = assert(c:selector_parse('/x=1'))
methods(s, {"capabilities", "is_empty", "root", "write_json"})
methods(s:root(), {"kind", "child_count", "child", "string_term", "string_term_any",
  "range_term", "date_term", "in_term", "in_term_any", "exists_path"})
methods(assert(c:projection_parse("/x")), {"path_count", "path"})
methods(assert(c:mutation_parse("/y=2")), {"count"})
local module_names = {}
for name in pairs(core) do module_names[#module_names + 1] = name end
table.sort(module_names)
assert(table.concat(module_names, ",") == "new,path_is_regular_file,status_string,version")

-- Explicit finalization is idempotent, and a cursor cannot outlive its selector.
local tree = assert(c:selector_build_not(s))
local root, child = tree:root(), tree:root():child(1)
finalize(tree); finalize(tree)
getmetatable(tree).__gc(tree)
expired(root, "kind"); expired(child, "string_term"); expired(root, "child_count")
finalize(c); finalize(c)
expired(s, "write_json"); expired(c, "version")
finalize(s); finalize(s)

local data = '{"x":1}\n'
for _, method in ipairs({"stream_apply", "stream_apply_spooled"}) do
  for _, kind in ipairs({"client", "selector", "projection", "mutation"}) do
    if method == "stream_apply_spooled" or kind == "client" or kind == "selector" then
      for _, point in ipairs({"reader", "decision", "value", "writer"}) do
        local client = assert(core.new())
        local selector = assert(client:selector_parse('/x=1'))
        local projection = assert(client:projection_parse("/x"))
        local mutation = assert(client:mutation_parse("/y=2"))
        local target = ({client = client, selector = selector, projection = projection, mutation = mutation})[kind]
        local hit, read, saved = false, false, nil
        local function close(where)
          if point == where and not hit then hit = true; finalize(target); finalize(target) end
        end
        local result, err = client[method](client, function()
          close("reader")
          if read then return nil end
          read = true
          return data
        end, selector, {
          input_is_compact = true,
          projection = method == "stream_apply_spooled" and projection or nil,
          mutation = method == "stream_apply_spooled" and mutation or nil,
          output_mode = method == "stream_apply_spooled" and "projection_then_mutation" or "selected_record",
          on_decision = function() close("decision"); return true end,
          on_value = function(value) saved = value; close("value"); return true end,
          writer = function() close("writer"); return true end,
          range_writer = function(offset, length, writer)
            return writer:write(data:sub(offset + 1, offset + length))
          end,
        })
        assert(hit, method .. ":" .. kind .. ":" .. point .. " callback missing")
        failed(result, err)
        if saved then expired(saved, "size") end
        if kind ~= "client" then
          assert(type(client:version()) == "string")
          assert(client:apply_string_spooled(nil, data).records_seen == 1)
        end
      end
    end
  end
end

-- Range writer and nested value writers expire even when callbacks throw.
for _, kind in ipairs({"client", "selector"}) do
  local client = assert(core.new())
  local selector = assert(client:selector_parse('/x=1'))
  local saved
  local read = false
  failed(client:stream_apply(function()
    if read then return nil end
    read = true
    return data
  end, selector, {
    input_is_compact = true, output_mode = "selected_record",
    range_writer = function(_, _, writer)
      saved = writer
      methods(writer, {"write"})
      finalize(kind == "client" and client or selector)
      return writer:write(data)
    end,
    writer = function() return true end,
  }))
  assert(saved); expired(saved, "write", data)
end
do
  local client = assert(core.new())
  local read, saved = false, nil
  local result, err = client:stream_apply_spooled(function()
    if read then return nil end
    read = true
    return data
  end, nil, {on_value = function(value)
    saved = value
    methods(value, {"size", "write_to"})
    return value:write_to(function() error("primary callback failure") end)
  end})
  failed(result, err)
  assert(err.message:find("primary callback failure", 1, true))
  expired(saved, "size")
end

-- Ordinary same-client nesting remains supported; unrelated GC cannot abort it.
do
  local client = assert(core.new())
  local unused = assert(client:selector_parse('/unused=1'))
  local read = false
  local result = assert(client:stream_apply_spooled(function()
    if read then return nil end
    read = true
    return data
  end, nil, {on_value = function()
    package.loaded["lql.core"] = nil
    assert(require("lql.core").version() == core.version())
    assert(client:version())
    local nested = assert(client:selector_parse('/x=1'))
    assert(client:apply_string_spooled(nested, data).records_matched == 1)
    finalize(unused)
    return true
  end}))
  assert(result.records_seen == 1)
end

-- Persistent file callbacks retain the mutation while its GC detaches userdata.
for _, point in ipairs({"open", "read"}) do
  local client = assert(core.new())
  local mutation
  mutation = assert(client:mutation_parse({"textfile:/payload=virtual"}, {
    file_value_open = function()
      if point == "open" then finalize(mutation) end
      local read = false
      return function()
        if point == "read" then finalize(mutation) end
        if read then return nil end
        read = true
        return "payload"
      end
    end,
  }))
  failed(client:apply_string_spooled(nil, '{}\n', {mutation = mutation}))
  assert(client:version())
end
do
  local client = assert(core.new())
  failed(client:mutation_parse({"time:/when=NOW"}, {time_now = function()
    finalize(client)
    return 1700000000
  end}))
end

-- An options getter can finalize its receiver before a filesystem side effect.
do
  local client = assert(core.new())
  local path = assert(os.getenv("TMPDIR")) .. "/lua-lifetime-fixture.ndjson"
  local file = assert(io.open(path, "wb")); file:write(data); file:close()
  local options = setmetatable({}, {__index = function(_, key)
    if key == "matched_only" then finalize(client); return true end
  end})
  failed(client:rewrite_file_inline_spooled('/x=2', path, options))
  file = assert(io.open(path, "rb")); assert(file:read("a") == data); file:close()
  assert(os.remove(path))
end
collectgarbage("collect")
print("Lua ownership, finalization, nested callbacks and existing API surface passed")
