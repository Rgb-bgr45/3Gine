-- 3sics demo: balls and crates fall onto a floor. Needs the "3sics" addon (listed in project.json).
engine.camera{ yaw=0.6, pitch=0.35, dist=16, target={0,2,0} }
engine.spawn{ name="Floor", mesh="cube", pos={0,-0.5,0}, scale={40,1,40}, color={0.42,0.66,0.38}, locked=true, script="physics_demo" }
engine.spawn{ name="Ball", mesh="sphere", pos={-2,4,0}, color={0.4,0.7,0.95}, shadow=1.2, material="plastic" }
engine.spawn{ name="Ball", mesh="sphere", pos={0,7,0.3}, color={0.95,0.5,0.4}, shadow=1.2, material="plastic" }
engine.spawn{ name="Ball", mesh="sphere", pos={2,10,0}, color={0.95,0.85,0.3}, shadow=1.2, material="plastic" }
engine.spawn{ name="Crate", mesh="cube", pos={-1,3,2}, color={0.6,0.4,0.25}, shadow=1.4 }
engine.spawn{ name="Crate", mesh="cube", pos={1.5,6,-1}, color={0.6,0.4,0.25}, shadow=1.4 }
engine.spawn{ name="BoingButton", mesh="button", pos={50,88,0}, scale={24,12,1}, color={0.8,0.5,0.2}, text="Boing!", script="boing" }
engine.spawn{ name="BackButton", mesh="button", pos={85,88,0}, scale={24,12,1}, color={0.3,0.65,0.4}, text="Back", script="go_main" }
engine.spawn{ name="Title", mesh="label", pos={50,8,0}, scale={40,7,1}, color={1,1,1}, text="3sics physics" }
