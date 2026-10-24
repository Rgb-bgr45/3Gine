-- Example 3gine addon. Copy to  Documents/3gine/addons/spin_all.lua  (rename to spin_all.lua.disable to turn it off).
-- An addon is a Lua file that returns a table with optional start() / update(dt).
local M = {}
function M.start()
  engine.log("spin_all addon loaded")
end
function M.update(dt)
  for _, id in ipairs(engine.find_all("Cube")) do
    engine.set_rot(id, engine.get_rot(id) + dt)
  end
end
return M
