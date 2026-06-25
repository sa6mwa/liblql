local lql = {}

local client = {}
client.__index = client

local function shell_quote(value)
  return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
end

local function read_file(path)
  local file = io.open(path, "rb")
  if not file then
    return nil
  end
  local data = file:read("*a")
  file:close()
  return data or ""
end

local function write_file(path, data)
  local file = io.open(path, "wb")
  if not file then
    return nil, "failed to open temporary file"
  end
  file:write(data)
  file:close()
  return true
end

local function remove_file(path)
  if path then
    os.remove(path)
  end
end

local function status_ok(status)
  return status == true or status == 0
end

local function status_code(status)
  if status == true then
    return 0
  end
  if type(status) == "number" then
    return status
  end
  return 1
end

local function command_for(clql, args)
  local parts = {shell_quote(clql)}
  for i = 1, #args do
    parts[#parts + 1] = shell_quote(args[i])
  end
  return table.concat(parts, " ")
end

local function run_capture(clql, args, input)
  local out_path = os.tmpname()
  local err_path = os.tmpname()
  local in_path
  local command = command_for(clql, args)
  if input ~= nil then
    in_path = os.tmpname()
    local ok, err = write_file(in_path, input)
    if not ok then
      remove_file(out_path)
      remove_file(err_path)
      remove_file(in_path)
      return nil, {status = 1, stderr = err or ""}
    end
    command = command .. " < " .. shell_quote(in_path)
  end
  local status = os.execute(command .. " > " .. shell_quote(out_path) ..
                              " 2> " .. shell_quote(err_path))
  local stdout = read_file(out_path) or ""
  local stderr = read_file(err_path) or ""
  remove_file(out_path)
  remove_file(err_path)
  remove_file(in_path)
  if status_ok(status) then
    return stdout, nil
  end
  return nil, {status = status_code(status), stdout = stdout, stderr = stderr}
end

local function add_projection_args(args, fields)
  for i = 1, #fields do
    args[#args + 1] = "-f"
    args[#args + 1] = fields[i]
  end
end

local function add_mutation_args(args, mutations)
  for i = 1, #mutations do
    args[#args + 1] = "-m"
    args[#args + 1] = mutations[i]
  end
end

function lql.new(options)
  options = options or {}
  return setmetatable({clql = options.clql or os.getenv("CLQL_PATH") or "clql"},
                      client)
end

function client:matches_json(selector, json)
  local _, err = run_capture(self.clql, {selector}, json)
  if err == nil then
    return true, nil
  end
  if err.stderr == "" then
    return false, nil
  end
  return nil, err
end

function client:select_json(selector, json, options)
  options = options or {}
  local args = {}
  if options.compact then
    args[#args + 1] = "-c"
  end
  args[#args + 1] = selector
  return run_capture(self.clql, args, json)
end

function client:select_file(selector, path, options)
  options = options or {}
  local args = {}
  if options.compact then
    args[#args + 1] = "-c"
  end
  args[#args + 1] = selector
  args[#args + 1] = path
  return run_capture(self.clql, args)
end

function client:project_file(selector, path, fields, options)
  options = options or {}
  local args = {}
  if options.compact then
    args[#args + 1] = "-c"
  end
  add_projection_args(args, fields)
  args[#args + 1] = selector
  args[#args + 1] = path
  return run_capture(self.clql, args)
end

function client:project_json(selector, json, fields, options)
  options = options or {}
  local args = {}
  if options.compact then
    args[#args + 1] = "-c"
  end
  add_projection_args(args, fields)
  args[#args + 1] = selector
  return run_capture(self.clql, args, json)
end

function client:mutate_file(selector, path, mutations, options)
  options = options or {}
  local args = {}
  add_mutation_args(args, mutations)
  if options.matches_only then
    args[#args + 1] = "-M"
  end
  if options.enable_file_mutations then
    args[#args + 1] = "-F"
  end
  args[#args + 1] = selector
  args[#args + 1] = path
  return run_capture(self.clql, args)
end

function client:mutate_json(selector, json, mutations, options)
  options = options or {}
  local args = {}
  add_mutation_args(args, mutations)
  if options.matches_only then
    args[#args + 1] = "-M"
  end
  if options.enable_file_mutations then
    args[#args + 1] = "-F"
  end
  args[#args + 1] = selector
  return run_capture(self.clql, args, json)
end

return lql
