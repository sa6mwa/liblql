local core = require("lql.core")
local function expired(handle, method, ...)
  assert(not pcall(handle[method], handle, ...), "closed handle remained usable")
end
local function failure(value, err, message)
  assert(value == nil and err and type(err.status) == "number", "expected structured failure")
  if message then assert(err.message:find(message, 1, true), err.message) end
end
local function owned(client)
  return assert(client:selector_parse('/x=1')),
         assert(client:projection_parse("/x")),
         assert(client:mutation_parse("/y=2"))
end

-- Explicit close, deterministic scope exit, and exceptional scope exit.
do
  local c = assert(core.new())
  local s, p, m = owned(c)
  local cursor = s:root()
  for _, h in ipairs({s, p, m, c}) do assert(h:close() == true); assert(h:close() == true) end
  expired(cursor, "kind"); expired(s, "root"); expired(p, "path_count")
  expired(m, "count"); expired(c, "version")
end
for _, throwing in ipairs({false, true}) do
  local saved = {}
  local sentinel = {}
  local ok, err = pcall(function()
    local c <close> = assert(core.new())
    local s <close> = assert(c:selector_parse('/x=1'))
    local p <close> = assert(c:projection_parse("/x"))
    local m <close> = assert(c:mutation_parse("/y=2"))
    saved = {c, s, p, m, s:root()}
    assert(s:close()) -- scope close after manual close is harmless
    if throwing then error(sentinel) end
  end)
  assert(ok == not throwing)
  if throwing then assert(err == sentinel) end
  for i, method in ipairs({"version", "root", "path_count", "count", "kind"}) do expired(saved[i], method) end
end
do
  local c <close> = assert(core.new())
  local s, p, m = owned(c)
  assert(c:close())
  expired(s, "root"); expired(p, "path_count"); expired(m, "count")
  assert(s:close()); assert(p:close()); assert(m:close())
end

-- Each native file pass gets a fresh reader and exactly one paired close.
local function callbacks(mode, owner_close)
  local state = {opened = 0, closed = 0, readers = {}}
  local function open()
    if mode == "open_error" then return nil, "open failed" end
    local sent = false
    local reader = function()
      if owner_close == "read" then assert(state.mutation:close()) end
      if mode == "read_error" or mode == "read_and_close_error" then return nil, "read failed" end
      if sent then return nil end
      sent = true
      return mode == "invalid_utf8" and "\255" or "payload"
    end
    state.opened = state.opened + 1
    state.readers[reader] = false
    if owner_close == "open" then assert(state.mutation:close()) end
    return reader
  end
  local function close(reader)
    assert(state.readers[reader] == false, "reader closed twice or wrong reader passed")
    state.readers[reader] = true
    state.closed = state.closed + 1
    if owner_close == "close" then assert(state.mutation:close()) end
    if mode == "close_throw" or mode == "read_and_close_error" or mode == "invalid_utf8" then error("close failed") end
    if mode == "close_error" then return nil, "close failed" end
    if mode == "close_false" then return false end
    return true
  end
  return state, {file_value_open = open, file_value_close = close}
end

for _, transient in ipairs({false, true}) do
  for _, mode in ipairs({"ok", "open_error", "read_error", "close_throw", "close_error", "close_false", "read_and_close_error", "invalid_utf8"}) do
    local c <close> = assert(core.new())
    local state, options = callbacks(mode)
    local mutation = {"textfile:/payload=virtual"}
    if not transient then mutation = assert(c:mutation_parse(mutation, options)) end
    options.mutation = mutation
    local result, err = c:apply_string_spooled(nil, '{}\n{}\n', options)
    assert(state.opened == state.closed, mode .. " left a reader open")
    if mode == "ok" then
      assert(result, err and err.message); assert(result.records_seen == 2)
      assert(state.opened >= 2)
    else
      failure(result, err)
      if mode == "read_and_close_error" then assert(err.message == "read failed") end
      if mode == "invalid_utf8" then assert(err.status_string == "json error" and err.message:find("UTF-8", 1, true), err.message) end
      if mode == "open_error" then assert(state.opened == 0) else assert(state.opened > 0) end
    end
    if not transient then assert(mutation:close()) end
  end
end

-- Capture options at parse time; close errors fail each public application.
for _, application in ipairs({"stream", "file", "inline"}) do
  local c <close> = assert(core.new())
  local state, options = callbacks("close_error")
  local m <close> = assert(c:mutation_parse({"textfile:/payload=virtual"}, options))
  options.file_value_close = function() error("replacement must not be called") end
  local result, err
  if application == "stream" then
    local read = false
    result, err = c:stream_apply_spooled(function()
      if read then return nil end; read = true; return '{}\n'
    end, nil, {mutation = m, output_mode = "mutation", writer = function() return true end})
  else
    local path = assert(os.getenv("TMPDIR")) .. "/lua-close-fixture.ndjson"
    local f = assert(io.open(path, "wb")); f:write('{}\n'); f:close()
    if application == "inline" then
      result, err = c:rewrite_file_inline_spooled(nil, path, {mutation = m})
    else
      result, err = c:filter_file_spooled(nil, path, {mutation = m})
    end
    assert(os.remove(path))
  end
  failure(result, err, "close failed")
  assert(state.opened > 0 and state.opened == state.closed)
end

-- A failure in the final native cleanup is observed after the rewrite commits.
do
  local c <close> = assert(core.new())
  local path = assert(os.getenv("TMPDIR")) .. "/lua-close-final-cleanup.ndjson"
  local passes
  for _, phase in ipairs({"control", "last-close-error"}) do
    local f = assert(io.open(path, "wb")); f:write('{}\n'); f:close()
    local state, options = callbacks("ok")
    local close = options.file_value_close
    options.file_value_close = function(reader)
      close(reader)
      if phase == "last-close-error" and state.closed == passes then return nil, "final close failed" end
      return true
    end
    local m <close> = assert(c:mutation_parse("textfile:/payload=virtual", options))
    local result, err = c:rewrite_file_inline_spooled(nil, path, {mutation = m})
    if phase == "control" then assert(result, err and err.message); passes = state.closed
    else failure(result, err, "final close failed") end
    assert(state.opened > 0 and state.opened == state.closed)
    f = assert(io.open(path, "rb")); assert(f:read("a"):find('"payload":"payload"', 1, true)); f:close()
  end
  assert(os.remove(path))
end

for _, stopping in ipairs({"stop", "cancel", "writer_error"}) do
  local c <close> = assert(core.new())
  local state, options = callbacks("ok")
  local m <close> = assert(c:mutation_parse("textfile:/payload=virtual", options))
  local read = false
  local result, err = c:stream_apply_spooled(function()
    if read then return nil end; read = true; return '{}\n{}\n'
  end, nil, {
    mutation = m, output_mode = "mutation",
    on_value = function() return stopping ~= "stop" end,
    cancelled = function() return stopping == "cancel" and state.opened > 0 end,
    writer = function()
      if stopping == "writer_error" and state.opened > 0 then return nil, "writer failed" end
      return true
    end,
  })
  assert(state.opened > 0 and state.opened == state.closed)
  if stopping == "writer_error" then failure(result, err, "writer failed")
  else assert(result, err and err.message); assert(result.stopped_early) end
end

-- A custom opener may own a real file without exposing C FILE pointers.
do
  local c <close> = assert(core.new())
  local path = assert(os.getenv("TMPDIR")) .. "/lua-close-payload.txt"
  local f = assert(io.open(path, "wb")); f:write("payload"); f:close()
  local files, opened = {}, {}
  local m <close> = assert(c:mutation_parse("textfile:/payload=" .. path, {
    file_value_open = function(reference)
      local file = assert(io.open(reference, "rb"))
      local reader = function(capacity) return file:read(capacity) end
      files[reader] = file; opened[#opened + 1] = file
      return reader
    end,
    file_value_close = function(reader)
      local file = assert(files[reader]); files[reader] = nil
      return file:close()
    end,
  }))
  assert(c:apply_string_spooled(nil, '{}\n{}\n', {mutation = m}))
  assert(#opened >= 2 and next(files) == nil)
  for _, file in ipairs(opened) do assert(io.type(file) == "closed file") end
  assert(os.remove(path))
end

-- Close may clear userdata ownership while callbacks still run on the Lua
-- thread which originally created a persistent mutation.
do
  local c <close> = assert(core.new())
  local weak = setmetatable({}, {__mode = "v"})
  local closed = false
  local thread = coroutine.create(function()
    local m
    m = assert(c:mutation_parse("textfile:/payload=virtual", {
      file_value_open = function()
        return function() assert(m:close()); collectgarbage("collect"); return "payload" end
      end,
      file_value_close = function()
        collectgarbage("collect")
        assert(weak[1] ~= nil, "originating Lua thread expired during cleanup")
        closed = true
      end,
    }))
    return m
  end)
  weak[1] = thread
  local ok, m = coroutine.resume(thread); assert(ok, m)
  thread = nil
  failure(c:apply_string_spooled(nil, '{}\n', {mutation = m}))
  assert(closed)
  collectgarbage("collect"); collectgarbage("collect")
  assert(weak[1] == nil)
end

-- Closing during open/read/close still permits the matching reader cleanup.
for _, point in ipairs({"open", "read", "close"}) do
  local c <close> = assert(core.new())
  local state, options = callbacks("ok", point)
  local m = assert(c:mutation_parse("textfile:/payload=virtual", options))
  state.mutation = m
  failure(c:apply_string_spooled(nil, '{}\n', {mutation = m}))
  assert(state.opened > 0 and state.opened == state.closed)
  expired(m, "count")
end

-- A persistent mutation releases captured callbacks on close, and a failed
-- cleanup can be retried by an operation using an otherwise open mutation.
do
  local c <close> = assert(core.new())
  local weak = setmetatable({}, {__mode = "v"})
  local attempts = 0
  local options = {file_value_open = function() return function() return nil end end,
    file_value_close = function() attempts = attempts + 1; if attempts == 1 then return nil, "retry close" end end}
  weak[1], weak[2] = options.file_value_open, options.file_value_close
  local m = assert(c:mutation_parse("textfile:/payload=virtual", options))
  options = nil
  failure(c:apply_string_spooled(nil, '{}\n', {mutation = m}))
  assert(c:apply_string_spooled(nil, '{}\n', {mutation = m}))
  assert(m:close())
  collectgarbage("collect")
  assert(weak[1] == nil and weak[2] == nil)
end

do
  local c <close> = assert(core.new())
  for _, options in ipairs({{file_value_close = function() end},
      {file_value_open = function() end, file_value_close = false}}) do
    local result, err = c:mutation_parse("/x=1", options)
    assert(result == nil and err.status_string == "invalid argument")
  end
end
print("Lua explicit/scoped close and paired file cleanup passed")
