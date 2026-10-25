-- Level 2: camera object, light (switches the renderer to Forward), metal material, virtual joystick.
engine.camera{ yaw=0.6, pitch=0.45, dist=12, target={0,1,0} }
engine.spawn{ name="Ground", mesh="cube", pos={0,-0.5,0}, scale={60,1,60}, color={0.55,0.5,0.7}, locked=true }
engine.spawn{ name="Donut", mesh="torus", pos={0,1,0}, scale={3,3,3}, color={0.95,0.6,0.3}, spin=1.2, shadow=2, material="metal", script="donut" }
engine.spawn{ name="Lamp", mesh="light", pos={2,4,2}, color={1,0.95,0.8}, intensity=2, range=14 }
engine.spawn{ name="Cam", mesh="camera", pos={7,5,9}, fov=0.9, lookid=2 }
engine.spawn{ name="Stick", mesh="joystick", pos={15,70,0}, scale={30,30,1} }
engine.spawn{ name="BackButton", mesh="button", pos={85,88,0}, scale={24,12,1}, color={0.3,0.65,0.4}, text="Back", script="go_main" }
engine.spawn{ name="Title", mesh="label", pos={50,8,0}, scale={40,7,1}, color={1,1,1}, text="Level 2 - use the joystick" }
