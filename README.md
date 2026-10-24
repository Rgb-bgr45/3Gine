# 3gine

Tiny stylized low-poly 3D engine + on-device editor for Android. C++17 (NativeActivity + EGL + OpenGL ES 3.0), Lua 5.4 scripting, Dear ImGui editor UI. Lua and ImGui are downloaded by CMake at build time.

**3D + engine = 3gine.**

## Two APKs (built by GitHub Actions)
- **3gine Editor** - project menu + editor.
- **Game APK** - plays the project named in `game/startup.txt`, no editor UI.

## Project menu
On start the editor shows the project list (like Godot): **Create** (asks for a name), **Open**, **Refresh**.
Projects live in `/sdcard/Documents/3gine/game/<name>/` (`scene.lua` + `scripts/*.lua`).
**First run:** Android settings -> Apps -> 3gine Editor -> Permissions -> "All files access".

## Editor
- Top bar: **Menu**, **Play** (sandbox test, **Stop** restores the scene), **Save**, **Build APK**, **+ Add**.
- **+ Add** shapes: Cube, Cone, Disc, Cylinder, Sphere, Pyramid, Prism, Wedge, Torus, Stairs. UI: **Button**, **Label**.
- Left: scene list. Right: inspector (name, position, size, rotation, colour, spin, sway, shadow, script).
- Tap an object in the 3D view to select it. Drag = orbit, pinch = zoom.
- UI elements (Button/Label) use Pos/Size in percent of the screen and a Text field.

## Scripts (Lua) on objects and buttons
Select an object or button -> **New** creates `scripts/<name>.lua` and opens the script editor (**Pick** reuses one, **Edit** reopens it).
The on-screen keyboard types into the editor; the symbol bar inserts ( ) { } [ ] " ' = etc. You can also edit the files in Termux.
Press **Play** to try it. Change the code, Save & Close, Play again.

```lua
-- script on a Button: every tap moves all objects named "Cube" 10 units along x
function on_click(self)
  for _, id in ipairs(engine.find_all("Cube")) do
    engine.move(id, 10, 0, 0)
  end
end

-- script on a 3D object
function start(self) end
function update(self, dt) engine.move(self, dt, 0, 0) end
```
`self` is the id of the object the script is attached to. Optional global script: `scripts/main.lua` with `start()` / `update(dt)`.

| Function | What it does |
|---|---|
| `engine.spawn{name=, mesh=, pos=, scale=, color=, rot=, spin=, sway=, shadow=, text=, script=}` | create an object, returns id. `mesh`: cube cone disc cylinder sphere pyramid prism wedge torus stairs button label |
| `engine.find(name)` / `engine.find_all(name)` | id / table of ids |
| `engine.destroy(id)` | remove |
| `engine.set_pos(id,x,y,z)` / `engine.get_pos(id)` / `engine.move(id,dx,dy,dz)` | position |
| `engine.set_scale` / `engine.get_scale`, `engine.set_color(id,r,g,b)`, `engine.set_rot` / `engine.get_rot` | change an object |
| `engine.set_text(id,s)` / `engine.get_text(id)` | button / label text |
| `engine.camera{yaw=,pitch=,dist=,target={x,y,z}}`, `engine.set_orbit(false)` | camera |
| `engine.touch()` -> down,x,y,dx,dy; `engine.screen()`; `engine.time()`; `engine.log(x)`; `engine.load(file)` | misc |

UI elements: `pos` and `scale` are percent of the screen (pos = centre). Built-in font is ASCII only (no Turkish letters yet).

## Editor -> game APK
1. In the editor press **Build APK** (saves and writes `startup.txt` with the project name).
2. In Termux, in your repo: `sh publish.sh` (copies `/sdcard/Documents/3gine/game` into the repo's `game/`, commits, pushes; run `termux-setup-storage` once).
3. GitHub Actions builds both APKs. Download `3gine-game-apk` from the Actions run (or Releases with a `v*` tag).

An APK cannot be compiled on the phone itself (needs the Android SDK/NDK), so GitHub compiles it.

## Layout
- `app/src/main/cpp/main.cpp` - engine, renderer, Lua bindings, editor, input
- `app/src/main/cpp/CMakeLists.txt` - native build
- `game/` - projects (`game/Demo` is included), `game/startup.txt` = project built into the game APK
- `publish.sh` - Termux helper

## License
MIT
