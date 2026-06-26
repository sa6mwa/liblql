local core = require("lql.core")

local lql = {}
local client = {}
client.__index = client

function lql.new(_options)
  return setmetatable({}, client)
end

function lql.has_core()
  return true
end

function client:matches_json(selector, json)
  return core.matches_json(selector, json)
end

function client:select_json(selector, json, options)
  return core.select_json(selector, json, options or {})
end

function client:select_file(selector, path, options)
  return core.select_file(selector, path, options or {})
end

function client:project_json(selector, json, fields, options)
  return core.project_json(selector, json, fields, options or {})
end

function client:project_file(selector, path, fields, options)
  return core.project_file(selector, path, fields, options or {})
end

function client:mutate_json(selector, json, mutations, options)
  return core.mutate_json(selector, json, mutations, options or {})
end

function client:mutate_file(selector, path, mutations, options)
  return core.mutate_file(selector, path, mutations, options or {})
end

return lql
