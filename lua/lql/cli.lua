local lql = require("lql")

local cli = {}

local function usage(file)
  file:write("usage: lql.lua [flags] selector... [data.json]\n")
  file:write("   or: lql.lua selector... < data.json\n")
  file:write("   or: cat data.json | lql.lua selector...\n\n")
  file:write("Selection flags:\n")
  file:write("  -c, --compact        compact output (currently always compact)\n")
  file:write("  -O, --or             combine selector arguments with OR\n")
  file:write("  -M, --matches-only   output only selector matches\n")
  file:write("      --count          output only the number of matches\n")
  file:write("  -h, --help           show help\n")
  file:write("  -v, --version        show version\n\n")
  file:write("Unsupported Go lql flags fail explicitly until supported:\n")
  file:write("  -m/--mutate, -f/--field, -t/--theme,\n")
  file:write("  -i/--inline, -w/--write, -F/--enable-file-mutations.\n\n")
  file:write("Reads strict NDJSON from file or stdin and writes compact matching\n")
  file:write("records to stdout, one JSON value per line. Root arrays are errors.\n")
end

local function exists_file(path)
  if path == "-" then
    return true
  end
  local f = io.open(path, "rb")
  if not f then
    return false
  end
  f:close()
  return true
end

local function unsupported_flag_with_value(arg)
  return arg == "-m" or arg == "--mutate" or arg:match("^%-%-mutate=") or
         arg == "-f" or arg == "--field" or arg:match("^%-%-field=") or
         arg == "-t" or arg == "--theme" or arg:match("^%-%-theme=")
end

local function unsupported_flag_no_value(arg)
  return arg == "-i" or arg == "--inline" or
         arg == "-w" or arg == "--write" or
         arg == "-F" or arg == "--enable-file-mutations"
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

function cli.main(argv)
  local count_only = false
  local or_mode = false
  local i = 1
  while i <= #argv do
    local a = argv[i]
    if a == "--" then
      i = i + 1
      break
    elseif a == "-h" or a == "--help" then
      usage(io.stdout)
      return 0
    elseif a == "-v" or a == "--version" then
      io.stdout:write(lql.version() .. "\n")
      return 0
    elseif a == "--count" then
      count_only = true
      i = i + 1
    elseif a == "-O" or a == "--or" then
      or_mode = true
      i = i + 1
    elseif a == "-c" or a == "--compact" or a == "-M" or
           a == "--matches-only" then
      i = i + 1
    elseif unsupported_flag_no_value(a) or unsupported_flag_with_value(a) then
      io.stderr:write("lql.lua: unsupported Go lql flag in current engine: " ..
                      a .. "\n")
      return 2
    elseif a:sub(1, 1) == "-" then
      io.stderr:write("lql.lua: unknown flag: " .. a .. "\n")
      usage(io.stderr)
      return 2
    else
      break
    end
  end

  local remaining = #argv - i + 1
  if remaining < 1 then
    usage(io.stderr)
    return 2
  end

  local path = "-"
  local selector_last = #argv
  if exists_file(argv[#argv]) then
    path = argv[#argv]
    selector_last = #argv - 1
  end
  if selector_last < i then
    usage(io.stderr)
    return 2
  end

  local selectors = {}
  for j = i, selector_last do
    selectors[#selectors + 1] = argv[j]
  end
  local expr = table.concat(selectors, or_mode and "," or "\n")

  local client, err = lql.new()
  if err then
    print_error("create context", err)
    return 1
  end
  local selector
  if or_mode then
    selector, err = client:selector_parse_or(expr)
  else
    selector, err = client:selector_parse(expr)
  end
  if err then
    print_error("parse selector", err)
    return 1
  end

  local result
  result, err = client:execute_file(selector, path,
                                   { count = count_only, stdout = true })
  if err then
    print_error("execute stream", err)
    return 1
  end
  if count_only then
    io.stdout:write(tostring(result.records_matched) .. "\n")
  end
  return 0
end

return cli
