local core = require("lql.core")

local lql = {}

lql.core = core
lql.new = core.new
lql.version = core.version

function lql.has_core()
  return core.core_loaded == true
end

return lql
