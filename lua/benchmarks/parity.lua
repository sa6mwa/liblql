local lql = require("lql")

local function die(message)
  io.stderr:write("lua parity benchmark: " .. message .. "\n")
  os.exit(1)
end

local function usage()
  io.stderr:write("usage: parity.lua MODE SELECTOR FIXTURE CANDIDATES\n")
  os.exit(2)
end

local mode = arg[1]
local expr = arg[2]
local fixture = arg[3]
local candidates = tonumber(arg[4] or "")

if not mode or not expr or not fixture or not candidates then
  usage()
end

local ok, message = pcall(function()
  local client = lql.new()
  local payloads = 0
  local payload_bytes = 0
  local matches = 0
  local result
  local err
  if mode == "plus_value_selector" or mode == "plus_value_plan" or
      mode == "plus_value_openjson_selector" or
      mode == "plus_value_openjson_plan" then
    result, err = client:each_match_file(expr, fixture, function(match)
      local payload, payload_err = match.json()
      if payload_err then
        die(payload_err.stderr or "payload read failed")
      end
      payloads = payloads + 1
      payload_bytes = payload_bytes + #payload
    end)
  elseif mode ~= "decision_only_selector" and mode ~= "decision_only_plan" then
    die("unsupported mode: " .. mode)
  else
    result, err = client:query_file(expr, fixture, function(decision)
      if decision.matched then
        matches = matches + 1
      end
    end)
  end
  if err then
    die(err.stderr or "query failed")
  end
  if result then
    matches = result.candidates_matched
  end
  io.write("candidates=", candidates, " matches=", matches, " payloads=",
           payloads, " payload_bytes=", payload_bytes, "\n")
end)

if not ok then
  die(message)
end
