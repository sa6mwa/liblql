local core = require("lql.core")

local lql = {}

lql.core = core
lql.new = core.new
lql.version = core.version
lql.status_string = core.status_string
lql.path_is_regular_file = core.path_is_regular_file

return lql
