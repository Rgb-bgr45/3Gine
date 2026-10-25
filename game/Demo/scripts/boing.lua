function on_click(self)
  for _, id in ipairs(engine.find_all("Ball")) do engine.physics_add_impulse(id, 0, 7, 0) end
end
