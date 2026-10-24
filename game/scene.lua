-- 3gine scene. The editor rewrites this file when you press Save.
engine.camera{ yaw=0.6, pitch=0.45, dist=22, target={0,1.5,0} }
math.randomseed(12345)

engine.spawn{ name="Ground", mesh="cube", pos={0,-0.5,0}, scale={160,1,160}, color={0.42,0.66,0.38} }

for i = 1, 20 do
  local x, z = (math.random() - 0.5) * 60, (math.random() - 0.5) * 60
  if math.abs(x) > 5 or math.abs(z) > 5 then
    local s = 0.8 + math.random() * 1.2
    local g = 0.85 + math.random() * 0.3
    engine.spawn{ name="Trunk", mesh="cube", pos={x, 0.6*s, z}, scale={0.35*s, 1.2*s, 0.35*s}, color={0.45,0.30,0.20} }
    engine.spawn{ name="Tree", mesh="cone", pos={x, 1.0*s, z}, scale={2.2*s, 3.2*s, 2.2*s},
                  color={0.22*g, 0.52*g, 0.30*g}, sway=0.4, shadow=2.6*s, rot=math.random()*6 }
  end
end

for i = 1, 8 do
  local s = 0.5 + math.random() * 0.9
  engine.spawn{ name="Rock", mesh="cube", pos={(math.random()-0.5)*50, 0.3*s, (math.random()-0.5)*50},
                scale={s, 0.6*s, 0.8*s}, rot=math.random()*6, color={0.55,0.56,0.60}, shadow=1.6*s }
end

engine.spawn{ name="Pedestal", mesh="cone", pos={0,0,0}, scale={3.5,1.2,3.5}, color={0.8,0.8,0.85}, shadow=4.2 }
engine.spawn{ name="Gold", mesh="cube", pos={0,1.8,0}, scale={1.8,1.8,1.8}, color={0.95,0.75,0.25}, spin=0.8 }
