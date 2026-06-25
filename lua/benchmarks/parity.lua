local lql = require("lql")

local function die(message)
  io.stderr:write("lua parity benchmark: " .. message .. "\n")
  os.exit(1)
end

local function count_lines_and_bytes(text)
  local lines = 0
  for _ in text:gmatch("\n") do
    lines = lines + 1
  end
  return lines, #text
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

local ok, message = pcall(function()
  local client = lql.new({clql = clql})
  local output, err = client:select_file(expr, fixture)
  if err then
    if err.stderr == "" then
      output = ""
    else
      die(err.stderr)
    end
  end
  local matches, output_bytes = count_lines_and_bytes(output)
  local payloads = 0
  local payload_bytes = 0
  if mode == "plus_value_selector" or mode == "plus_value_plan" or
      mode == "plus_value_openjson_selector" or
      mode == "plus_value_openjson_plan" then
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

if not ok then
  die(message)
end
