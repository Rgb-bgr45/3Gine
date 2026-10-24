-- Runs when you press Play (and in the exported game). start() once, update(dt) every frame.
local gold
local was_down = false

function start()
  gold = engine.find("Gold")          -- look an object up by the name shown in the editor
end

function update(dt)
  if gold then                        -- the gold cube bobs up and down
    local x, y, z = engine.get_pos(gold)
    engine.set_pos(gold, x, 1.8 + math.sin(engine.time() * 2) * 0.25, z)
  end

  local down = engine.touch()         -- tap the screen: drop a coloured cube
  if down and not was_down then
    engine.spawn{ name="Drop", mesh="cube", pos={(math.random()-0.5)*8, 3, (math.random()-0.5)*8},
                  scale={0.7,0.7,0.7}, color={math.random(), math.random(), math.random()},
                  spin=math.random()*3, shadow=1.0 }
  end
  was_down = down
end
