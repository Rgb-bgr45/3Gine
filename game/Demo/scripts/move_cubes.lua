-- Button script: every tap moves all objects named "Cube" 10 units along x.
function on_click(self)
  for _, id in ipairs(engine.find_all("Cube")) do
    engine.move(id, 10, 0, 0)
  end
end
