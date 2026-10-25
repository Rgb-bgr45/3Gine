-- 3sics: mid-level physics (gravity, AABB + sphere collisions, restitution, mass / impulse).
-- The physics itself is built into 3gine (C++). This file is the switch: rename it to 3sics.lua.disable to turn it off.
-- Scripts use: engine.physics_add_body(id, {shape="box"|"sphere", mass=1, restitution=0.3, static=false})
--   engine.physics_set_gravity(x,y,z)  engine.physics_add_impulse(id,x,y,z)  engine.physics_set_velocity(id,x,y,z)
--   engine.physics_get_velocity(id)    engine.physics_remove_body(id)         engine.physics_clear()
--   function on_collision(a, b) end    -- global callback when two bodies start touching
return {}
