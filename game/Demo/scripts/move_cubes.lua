-- Script of the button "MoveButton". self = this button's id.
-- Each tap moves every object named "Cube" 10 units along x. Change the numbers and press Play again.
function on_click(self)
  for _, id in ipairs(engine.find_all("Cube")) do
    engine.move(id, 10, 0, 0)
  end
end
