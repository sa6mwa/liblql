#!/usr/bin/env lua5.5

local lql = require("lql.core")

local function usage(file)
  file:write("usage: lql.lua [-m mutator...] [-f field...] selector... [data.json]\n")
  file:write("   or: lql.lua selector... < data.json\n")
  file:write("   or: cat data.json | lql.lua selector...\n\n")
  file:write("Selectors:\n")
  file:write("  LQL selector expressions (comma/newline separated).\n\n")
  file:write("Mutations:\n")
  file:write("  -m, --mutate expr    apply mutations to each JSON object in the input stream\n")
  file:write("  -i, --inline         write mutation output inline to a single input file\n")
  file:write("  -w, --write          alias of --inline\n")
  file:write("  -F, --enable-file-mutations\n")
  file:write("                       allow file:/textfile:/base64file: mutation values\n\n")
  file:write("Output:\n")
  file:write("  -f, --field /path    output only selected JSON Pointer fields (repeatable)\n")
  file:write("  -c, --compact        compact output (always compact; prettyx unsupported)\n")
  file:write("  -t, --theme theme    unsupported: lql.lua does not include prettyx themes\n")
  file:write("  -h, --help           show help\n")
  file:write("  -v, --version        show version\n")
  file:write("  -O, --or             combine selector arguments with OR\n")
  file:write("  -M, --matches-only   output only selector matches (even with -m)\n")
  file:write("      --count          output only the number of selector matches\n\n")
  file:write("Selector examples (shorthand):\n")
  file:write("  /status=\"open\"\n")
  file:write("  /status!=closed\n")
  file:write("  /progress>=50\n")
  file:write("  /timestamp>=\"2025-01-01T00:00:00Z\"\n")
  file:write("  /devices/0/status=\"online\"\n")
  file:write("  /labels/*=\"production\"\n")
  file:write("  /items[]/sku=\"ABC-123\"\n")
  file:write("  /items/**/sku=\"ABC-123\"\n")
  file:write("  /items/.../sku=\"ABC-123\"\n\n")
  file:write("Selector examples (full LQL):\n")
  file:write("  eq{field=/status,value=open}\n")
  file:write("  contains{field=/msg,value=timeout,ic=t}\n")
  file:write("  contains{field=/msg,any=timeout|degraded}\n")
  file:write("  icontains{field=/msg,value=timeout}\n")
  file:write("  icontains{field=/service,a=AUTH|EDGE}\n")
  file:write("  iprefix{field=/service,value=auth}\n")
  file:write("  date{field=/timestamp,after=2025-01-01,before=2025-02-01}\n")
  file:write("  date{f=/timestamp,since=yesterday}\n")
  file:write("  and.eq{field=/status,value=open},and.range{field=/progress,gte=50}\n")
  file:write("  or.eq{field=/region,value=us},or.eq{field=/region,value=eu}\n")
  file:write("  not.eq{field=/state,value=disabled}\n")
  file:write("  exists{/metadata/etag}\n\n")
  file:write("Invocation examples:\n")
  file:write("  lql.lua -O '/status=\"open\"' '/status=\"queued\"' data.json\n")
  file:write("  lql.lua --count '/status=\"open\"' data.json\n")
  file:write("  cat data.json | lql.lua '/items[]/sku=\"ABC-123\"'\n\n")
  file:write("File-backed mutation example:\n")
  file:write("  printf '{}\\n' | lql.lua -F \\\n")
  file:write("    -m '/filename=notes.txt' -m '/tags/kind=document' \\\n")
  file:write("    -m '/tags/source=local' -m 'textfile:/content=notes.txt'\n\n")
  file:write("Notes:\n")
  file:write("  contains/icontains accept value=... or any=/a=... (pipe-delimited).\n")
  file:write("  range comparisons accept numeric or datetime literals.\n")
  file:write("  date supports value/after/before/gt/gte/lt/lte; aliases a=after and b=before.\n")
  file:write("  only date{...,since=...} supports relative macros (now, today, yesterday).\n")
  file:write("  omitted values for contains/icontains/prefix/iprefix act as path assertions.\n\n")
  file:write("Reads strict NDJSON from file or stdin and writes compact matching\n")
  file:write("records to stdout, one JSON value per line. Root arrays are errors.\n")
  file:write("Projection and mutation output use liblql's explicitly spooled path\n")
  file:write("and may spill the current record to a temporary file.\n")
end

local function exists_file(path)
  if path == "-" then
    return true
  end
  return lql.path_is_regular_file(path)
end

local function take_value(argv, i, inline_value, flag)
  if inline_value ~= nil then
    return inline_value, i
  end
  if i + 1 > #argv then
    io.stderr:write("lql.lua: " .. flag .. " requires a value\n")
    return nil, i
  end
  return argv[i + 1], i + 1
end

local function long_value(arg, name)
  local prefix = name .. "="
  if arg:sub(1, #prefix) == prefix then
    return arg:sub(#prefix + 1)
  end
  return nil
end

local function bool_value(text)
  if text == nil then
    return true, true
  end
  if text == "1" or text == "true" or text == "True" or text == "TRUE" or
      text == "t" or text == "T" then
    return true, true
  end
  if text == "0" or text == "false" or text == "False" or text == "FALSE" or
      text == "f" or text == "F" then
    return false, true
  end
  return false, false
end

local function parse_long_bool(a, name, cfg, key)
  local value = long_value(a, name)
  if value == nil then
    return false, nil
  end
  local parsed, ok = bool_value(value)
  if not ok then
    io.stderr:write("lql.lua: invalid boolean value for " .. name .. "\n")
    return true, 2
  end
  cfg[key] = parsed
  return true, nil
end

local function parse_short_bool_value(a, pos)
  if a:sub(pos + 1, pos + 1) ~= "=" then
    return nil, false, nil
  end
  local value, ok = bool_value(a:sub(pos + 2))
  if not ok then
    io.stderr:write("lql.lua: invalid boolean value for short option: " ..
                    a:sub(pos, pos) .. "\n")
    return nil, true, 2
  end
  return value, true, nil
end

local function parse_short_cluster(argv, i, cfg)
  local a = argv[i]
  if #a <= 2 or a:sub(1, 1) ~= "-" or a:sub(1, 2) == "--" then
    return false, i, nil
  end
  local terminal
  local pos = 2
  while pos <= #a do
    local ch = a:sub(pos, pos)
    if ch == "O" or ch == "M" or ch == "c" or ch == "i" or ch == "w" or
        ch == "F" then
      local value, matched, err = parse_short_bool_value(a, pos)
      if err then
        return true, i, err
      end
      if matched then
        if ch == "O" then
          cfg.or_mode = value
        elseif ch == "M" then
          cfg.matches_only = value
        elseif ch == "c" then
          cfg.compact = value
        elseif ch == "i" or ch == "w" then
          cfg.inline = value
        else
          cfg.enable_file_mutations = value
        end
        return true, i, nil
      end
      if ch == "O" then
        cfg.or_mode = true
      elseif ch == "M" then
        cfg.matches_only = true
      elseif ch == "c" then
        cfg.compact = true
      elseif ch == "i" or ch == "w" then
        cfg.inline = true
      else
        cfg.enable_file_mutations = true
      end
      pos = pos + 1
    elseif ch == "h" then
      terminal = "help"
      pos = pos + 1
    elseif ch == "v" then
      if terminal ~= "help" then
        terminal = "version"
      end
      pos = pos + 1
    elseif ch == "m" or ch == "f" then
      local value
      if pos < #a and a:sub(pos + 1, pos + 1) == "=" then
        value = a:sub(pos + 2)
      elseif pos < #a then
        value = a:sub(pos + 1)
      else
        value, i = take_value(argv, i, nil, ch == "m" and "--mutate" or "--field")
        if value == nil then
          return true, i, 2
        end
      end
      if ch == "m" then
        cfg.mutations[#cfg.mutations + 1] = value
      else
        cfg.fields[#cfg.fields + 1] = value
      end
      return true, i, nil
    elseif ch == "t" then
      if pos == #a then
        local _
        _, i = take_value(argv, i, nil, "--theme")
        if _ == nil then
          return true, i, 2
        end
      end
      io.stderr:write("lql.lua: --theme is unsupported; prettyx is not linked\n")
      return true, i, 2
    else
      return false, i, nil
    end
  end
  if terminal == "help" then
    usage(io.stdout)
    return true, i, 0
  end
  if terminal == "version" then
    io.stdout:write(lql.version() .. "\n")
    return true, i, 0
  end
  return true, i, nil
end

local function print_error(context, err)
  local status = err and (err.status_string or err.status) or "error"
  local message = err and err.message or ""
  if message ~= "" then
    io.stderr:write("lql.lua: " .. context .. ": " .. tostring(status) ..
                    ": " .. tostring(message) .. "\n")
  else
    io.stderr:write("lql.lua: " .. context .. ": " .. tostring(status) .. "\n")
  end
end

local function parse_args(argv)
  local cfg = {
    mutations = {},
    fields = {},
    positionals = {},
    inline = false,
    compact = false,
    enable_file_mutations = false,
    count = false,
    or_mode = false,
    matches_only = false,
  }
  local i = 1
  while i <= #argv do
    local a = argv[i]
    if a == "--" then
      i = i + 1
      while i <= #argv do
        cfg.positionals[#cfg.positionals + 1] = argv[i]
        i = i + 1
      end
      break
    elseif a == "-h" or a == "--help" then
      usage(io.stdout)
      return nil, 0
    elseif a == "-v" or a == "--version" then
      io.stdout:write(lql.version() .. "\n")
      return nil, 0
    elseif a == "-c" or a == "--compact" then
      cfg.compact = true
      i = i + 1
    elseif long_value(a, "--compact") ~= nil then
      local matched, err = parse_long_bool(a, "--compact", cfg, "compact")
      if err then
        return nil, err
      end
      if matched then
        i = i + 1
      end
    elseif a == "-O" or a == "--or" then
      cfg.or_mode = true
      i = i + 1
    elseif long_value(a, "--or") ~= nil then
      local matched, err = parse_long_bool(a, "--or", cfg, "or_mode")
      if err then
        return nil, err
      end
      if matched then
        i = i + 1
      end
    elseif a == "-M" or a == "--matches-only" then
      cfg.matches_only = true
      i = i + 1
    elseif long_value(a, "--matches-only") ~= nil then
      local matched, err = parse_long_bool(a, "--matches-only", cfg, "matches_only")
      if err then
        return nil, err
      end
      if matched then
        i = i + 1
      end
    elseif a == "--count" then
      cfg.count = true
      i = i + 1
    elseif long_value(a, "--count") ~= nil then
      local value, ok = bool_value(long_value(a, "--count"))
      if not ok then
        io.stderr:write("lql.lua: invalid boolean value for --count\n")
        return nil, 2
      end
      cfg.count = value
      i = i + 1
    elseif a == "-i" or a == "--inline" or a == "-w" or a == "--write" then
      cfg.inline = true
      i = i + 1
    elseif long_value(a, "--inline") ~= nil or long_value(a, "--write") ~= nil then
      local name = long_value(a, "--inline") ~= nil and "--inline" or "--write"
      local text = long_value(a, name)
      local value, ok = bool_value(text)
      if not ok then
        io.stderr:write("lql.lua: invalid boolean value for " .. name .. "\n")
        return nil, 2
      end
      cfg.inline = value
      i = i + 1
    elseif a == "-F" or a == "--enable-file-mutations" then
      cfg.enable_file_mutations = true
      i = i + 1
    elseif long_value(a, "--enable-file-mutations") ~= nil then
      local value, ok = bool_value(long_value(a, "--enable-file-mutations"))
      if not ok then
        io.stderr:write("lql.lua: invalid boolean value for --enable-file-mutations\n")
        return nil, 2
      end
      cfg.enable_file_mutations = value
      i = i + 1
    elseif a == "-m" or a == "--mutate" or long_value(a, "--mutate") ~= nil then
      local value
      value, i = take_value(argv, i, long_value(a, "--mutate"), "--mutate")
      if value == nil then
        return nil, 2
      end
      cfg.mutations[#cfg.mutations + 1] = value
      i = i + 1
    elseif a:sub(1, 2) == "-m" and #a > 2 then
      if a:sub(3, 3) == "=" then
        cfg.mutations[#cfg.mutations + 1] = a:sub(4)
      else
        cfg.mutations[#cfg.mutations + 1] = a:sub(3)
      end
      i = i + 1
    elseif a == "-f" or a == "--field" or long_value(a, "--field") ~= nil then
      local value
      value, i = take_value(argv, i, long_value(a, "--field"), "--field")
      if value == nil then
        return nil, 2
      end
      cfg.fields[#cfg.fields + 1] = value
      i = i + 1
    elseif a:sub(1, 2) == "-f" and #a > 2 then
      if a:sub(3, 3) == "=" then
        cfg.fields[#cfg.fields + 1] = a:sub(4)
      else
        cfg.fields[#cfg.fields + 1] = a:sub(3)
      end
      i = i + 1
    elseif a == "-t" or a == "--theme" or long_value(a, "--theme") ~= nil then
      if long_value(a, "--theme") == nil then
        local _
        _, i = take_value(argv, i, nil, "--theme")
        if _ == nil then
          return nil, 2
        end
      end
      io.stderr:write("lql.lua: --theme is unsupported; prettyx is not linked\n")
      return nil, 2
    elseif a:sub(1, 1) == "-" then
      local matched, next_i, err = parse_short_cluster(argv, i, cfg)
      if err ~= nil then
        return nil, err
      end
      if matched then
        i = next_i + 1
      else
        io.stderr:write("lql.lua: unknown flag: " .. a .. "\n")
        usage(io.stderr)
        return nil, 2
      end
    else
      cfg.positionals[#cfg.positionals + 1] = a
      i = i + 1
    end
  end
  return cfg, nil
end

local function split_inputs(cfg)
  local selectors = {}
  local inputs = {}
  if #cfg.mutations == 0 then
    for i, item in ipairs(cfg.positionals) do
      if i == #cfg.positionals and (item == "-" or exists_file(item)) then
        inputs[#inputs + 1] = item
      else
        selectors[#selectors + 1] = item
      end
    end
  else
    for _, item in ipairs(cfg.positionals) do
      if item == "-" or exists_file(item) then
        inputs[#inputs + 1] = item
      else
        selectors[#selectors + 1] = item
      end
    end
  end
  if #inputs == 0 then
    inputs[#inputs + 1] = "-"
  end
  return selectors, inputs
end

local function run_once(client, selector, input, options)
  local result, err
  if options and options.inline then
    result, err = client:rewrite_file_inline_spooled(selector, input, options)
  else
    result, err = client:filter_file_spooled(selector, input, options)
  end
  if err then
    print_error(options and options.inline and "rewrite file" or "filter file", err)
    return nil
  end
  return result
end

local function main(argv)
  local cfg, early = parse_args(argv)
  if early ~= nil then
    return early
  end
  local selectors, inputs = split_inputs(cfg)
  if #selectors == 0 and #cfg.mutations == 0 and #cfg.fields == 0 then
    usage(io.stderr)
    return 2
  end
  if cfg.inline and #cfg.mutations == 0 then
    io.stderr:write("lql.lua: inline mode requires mutations\n")
    return 2
  end
  if cfg.inline and cfg.count then
    io.stderr:write("lql.lua: inline mutation cannot be combined with --count\n")
    return 2
  end
  if cfg.inline and (#inputs ~= 1 or inputs[1] == "-") then
    io.stderr:write("lql.lua: inline mode requires a single JSON file\n")
    return 2
  end

  local client, err = lql.new()
  if err then
    print_error("create context", err)
    return 1
  end
  local selector
  if #selectors ~= 0 then
    local expr = table.concat(selectors, cfg.or_mode and "," or "\n")
    if cfg.or_mode then
      selector, err = client:selector_parse_or(expr)
    else
      selector, err = client:selector_parse(expr)
    end
    if err then
      print_error("parse selector", err)
      return 1
    end
  end

  local projection
  if #cfg.fields ~= 0 then
    projection, err = client:projection_parse(cfg.fields)
    if err then
      print_error("parse projection", err)
      return 1
    end
  end
  local mutation
  if #cfg.mutations ~= 0 then
    mutation, err = client:mutation_parse(cfg.mutations, {
      enable_file_mutations = cfg.enable_file_mutations,
      file_value_base_dir = ".",
    })
    if err then
      print_error("parse mutation", err)
      return 1
    end
  end

  local matched = 0
  local seen = 0
  if cfg.inline then
    local path = inputs[1]
    local result = run_once(client, selector, path, {
      projection = projection,
      mutation = mutation,
      matched_only = cfg.matches_only,
      inline = true,
    })
    if not result then
      return 1
    end
    return 0
  end

  for _, input in ipairs(inputs) do
    local matched_only = true
    if mutation then
      matched_only = cfg.matches_only
    end
    local result = run_once(client, selector, input, {
      projection = projection,
      mutation = mutation,
      matched_only = matched_only,
      count = cfg.count,
      stdout = not cfg.count,
    })
    if not result then
      return 1
    end
    seen = seen + result.records_seen
    matched = matched + result.records_matched
  end
  if mutation and seen == 0 then
    io.stderr:write("lql.lua: no JSON input\n")
    return 1
  end
  if cfg.count then
    io.stdout:write(tostring(matched) .. "\n")
  end
  return 0
end

os.exit(main(arg))
