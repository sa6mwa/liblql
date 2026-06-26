local core = require("lql.core")

local lql = {}

function lql.new(_options)
  return core.new()
end

function lql.has_core()
  return true
end

return lql
