local function die(message)
  io.stderr:write("lua parity benchmark: " .. message .. "\n")
  os.exit(1)
end

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

local function remove_file(path)
  if path then
    os.remove(path)
  end
end

local function run_to_file(command, out_path, err_path)
  local status = os.execute(command .. " > " .. shell_quote(out_path) ..
                              " 2> " .. shell_quote(err_path))
  if status == true or status == 0 then
    return
  end
  local err = read_file(err_path) or ""
  if err == "" then
    return
  end
  die("command failed: " .. command .. "\n" .. err)
end

local function count_lines_and_bytes(path)
  local file = io.open(path, "rb")
  if not file then
    die("failed to open output file")
  end
  local lines = 0
  local bytes = 0
  while true do
    local chunk = file:read(8192)
    if not chunk then
      break
    end
    bytes = bytes + #chunk
    for _ in chunk:gmatch("\n") do
      lines = lines + 1
    end
  end
  file:close()
  return lines, bytes
end

local function usage()
  io.stderr:write("usage: parity.lua MODE SELECTOR FIXTURE CANDIDATES CLQL\n")
  os.exit(2)
end

local mode = arg[1]
local expr = arg[2]
local fixture = arg[3]
local candidates = tonumber(arg[4] or "")
local clql = arg[5]

if not mode or not expr or not fixture or not candidates or not clql then
  usage()
end

local out_path = os.tmpname()
local err_path = os.tmpname()

local ok, message = pcall(function()
  run_to_file(shell_quote(clql) .. " " .. shell_quote(expr) .. " " ..
                shell_quote(fixture), out_path, err_path)
  local matches, output_bytes = count_lines_and_bytes(out_path)
  local payloads = 0
  local payload_bytes = 0
  if mode == "plus_value_selector" or mode == "plus_value_plan" then
    payloads = matches
    payload_bytes = output_bytes - matches
    if payload_bytes < 0 then
      payload_bytes = 0
    end
  elseif mode ~= "decision_only_selector" and mode ~= "decision_only_plan" then
    die("unsupported mode: " .. mode)
  end
  io.write("candidates=", candidates, " matches=", matches, " payloads=",
           payloads, " payload_bytes=", payload_bytes, "\n")
end)

remove_file(out_path)
remove_file(err_path)

if not ok then
  die(message)
end
