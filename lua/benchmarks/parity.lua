local lql = require("lql")

local function die(message)
  io.stderr:write("lua parity benchmark: " .. message .. "\n")
  os.exit(1)
end

local function usage()
  io.stderr:write("usage: parity.lua MODE SELECTOR FIXTURE CANDIDATES [SUBMODE]\n")
  os.exit(2)
end

local mode = arg[1]
local expr = arg[2]
local fixture = arg[3]
local candidates = tonumber(arg[4] or "")
local submode = arg[5] or "warmup_included"

if not mode or not expr or not fixture or not candidates then
  usage()
end

if submode ~= "warmup_included" and submode ~= "steady_state" then
  die("unsupported submode: " .. submode)
end

local function run_once(client, selector_arg)
  local payloads = 0
  local payload_bytes = 0
  local matches = 0
  local result
  local err
  local mutated
  local source_file
  local mutation_arg = {"/bench/touched=true"}
  local mutation_plan
  local function read_source()
    local chunk = source_file:read(1024)
    if not chunk or #chunk == 0 then
      return nil
    end
    return chunk
  end
  local function count_lines(text)
    local count = 0
    local pos = 1
    while true do
      local next_pos = string.find(text, "\n", pos, true)
      if not next_pos then
        break
      end
      count = count + 1
      pos = next_pos + 1
    end
    return count
  end
  if mode == "plus_value_selector" or mode == "plus_value_plan" or
      mode == "plus_value_openjson_selector" or
      mode == "plus_value_openjson_plan" then
    result, err = client:each_match_file(selector_arg, fixture, function(match)
      local ok, payload_err = match.write_json(function(chunk)
        payload_bytes = payload_bytes + #chunk
      end)
      if payload_err then
        die(payload_err.stderr or "payload read failed")
      end
      payloads = payloads + 1
    end)
  elseif mode == "mutate_file_selector" or mode == "mutate_file_plan" then
    if mode == "mutate_file_plan" then
      mutation_plan, err = client:mutation_plan_parse(mutation_arg)
      if err then
        die(err.stderr or "mutation plan parse failed")
      end
      mutation_arg = mutation_plan
    end
    mutated, err = client:mutate_file(selector_arg, fixture, mutation_arg,
                                      {compact = true, matches_only = true})
    if err then
      die(err.stderr or "mutate_file failed")
    end
    matches = count_lines(mutated)
  elseif mode == "mutate_source_selector" then
    source_file = assert(io.open(fixture, "rb"))
    mutated, err = client:mutate_source(selector_arg, read_source,
                                        mutation_arg,
                                        {compact = true, matches_only = true})
    source_file:close()
    if err then
      die(err.stderr or "mutate_source failed")
    end
    matches = count_lines(mutated)
  elseif mode == "plus_value_source_selector" then
    source_file = assert(io.open(fixture, "rb"))
    result, err = client:each_match_source(expr, read_source, function(match)
      local ok, payload_err = match.write_json(function(chunk)
        payload_bytes = payload_bytes + #chunk
      end)
      if payload_err then
        source_file:close()
        die(payload_err.stderr or "payload read failed")
      end
      payloads = payloads + 1
    end)
    source_file:close()
  elseif mode ~= "decision_only_selector" and mode ~= "decision_only_plan" then
    if mode ~= "decision_only_source_selector" and
        mode ~= "reuse_selector" and
        mode ~= "reparse_selector_each_run" then
      die("unsupported mode: " .. mode)
    end
    source_file = assert(io.open(fixture, "rb"))
    if mode == "decision_only_source_selector" then
      result, err = client:query_source(selector_arg, read_source,
                                        function(decision)
        if decision.matched then
          matches = matches + 1
        end
      end)
    else
      source_file:close()
      source_file = nil
      result, err = client:query_file(selector_arg, fixture, function(decision)
        if decision.matched then
          matches = matches + 1
        end
      end)
    end
    if source_file then
      source_file:close()
    end
  else
    result, err = client:query_file(selector_arg, fixture, function(decision)
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
  return matches, payloads, payload_bytes
end

local ok, message = pcall(function()
  local client = lql.new()
  local selector_arg = expr
  local matches
  local payloads
  local payload_bytes
  local start
  local elapsed_ns
  if mode == "reuse_selector" or mode == "decision_only_plan" or
      mode == "plus_value_plan" or mode == "plus_value_openjson_plan" then
    local selector, selector_err = client:selector_parse(expr)
    if selector_err then
      die(selector_err.stderr or "selector parse failed")
    end
    selector_arg = selector
  end
  if submode == "steady_state" then
    run_once(client, selector_arg)
  end
  start = os.clock()
  matches, payloads, payload_bytes = run_once(client, selector_arg)
  elapsed_ns = math.floor(((os.clock() - start) * 1000000000) + 0.5)
  io.write("candidates=", candidates, " matches=", matches, " payloads=",
           payloads, " payload_bytes=", payload_bytes, " elapsed_ns=",
           elapsed_ns, "\n")
end)

if not ok then
  die(message)
end
