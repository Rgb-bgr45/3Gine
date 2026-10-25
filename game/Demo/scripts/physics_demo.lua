-- Runs on the Floor. Registers the bodies with 3sics (the floor is static).
function start(self)
  engine.physics_set_gravity(0, -9.8, 0)
  engine.physics_add_body(self, { static = true })
  for _, id in ipairs(engine.find_all("Ball")) do
    engine.physics_add_body(id, { shape = "sphere", mass = 1, restitution = 0.8 })
  end
  for _, id in ipairs(engine.find_all("Crate")) do
    engine.physics_add_body(id, { mass = 2, restitution = 0.2 })
  end
end

function on_collision(a, b)
  -- engine.log(engine.get_name(a) .. " hit " .. engine.get_name(b))
end
