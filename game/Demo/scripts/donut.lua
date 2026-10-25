-- Moves the donut with the virtual joystick (globals joystick_x / joystick_y, -1..1).
function update(self, dt)
  engine.move(self, (joystick_x or 0) * dt * 4, 0, -(joystick_y or 0) * dt * 4)
end
