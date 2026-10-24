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
<<<<<<< HEAD

## Layout
- `app/src/main/cpp/main.cpp` - engine, renderer, Lua bindings, editor, input
=======

## Hierarchy, scenes, folders
- **Parent / child:** select an object -> `Parent: Set`. A child follows its parent's position, rotation and scale: `world = parentWorld x local` (matrix product). Loops are refused; deleting a parent deletes its children.
- **Folders:** type a name in the `Folder` field, the scene list groups objects by folder. **Hide** hides an object in the editor only, **Lock** stops it from being picked or edited.
- **Scenes:** top bar `Scenes` switches scenes or makes a new one (`scenes/<name>.lua`, the first scene is `scene.lua` = "main"). In a script: `engine.load_scene("Level2")`.
- Extra Lua: `get_name`, `set_parent`, `get_parent`, `get_world_pos`, `get_distance(a,b)`, `move_towards(id,x,y,z,speed)` (true when reached), `check_raycast(x,y,z,dx,dy,dz)` -> id, distance, `load_scene(name)`.

## Addons (mods)
- Folder: `/sdcard/Documents/3gine/addons/` (fallback: the app's private folder). Top bar `Mods` lists them: Enable/Disable renames the file with/without `.disable`; disabled files are ignored at start.
- **.lua** addons return a table: `{ start=function() end, update=function(dt) end }` (see `examples/addons/spin_all.lua`).
- **.so** addons are native (arm64): export `gine_addon_init(const GineAPI*)` from `gine_addon.h` (see `examples/addons/native_example.cpp`). The library is copied into the app's private folder before loading.
- **.dll / .gpp** are PC formats: they are listed as "not supported on Android".
- **Required:** tick `Required` and the addon is written to the project's `project.json`. Play refuses to start when a required addon is missing or disabled and tells you which. The game APK shows the same message instead of crashing. On **Build APK** required `.lua` addons are copied into the project (`.so` addons are not bundled).
- **crash.log** (in `Documents/3gine/`): Lua errors from addons/scripts and native crashes, with the name of the mod that was running.

Not done yet (next steps): camera object, lights/shaders/materials, 3sics physics, virtual joystick, move gizmo with snap, .3ne backup, language screen, "Prepare for APK" zip, asset store.

## Layout
- `app/src/main/cpp/main.cpp` - engine, renderer, Lua bindings, editor, input, addons
- `app/src/main/cpp/gine_addon.h` - native addon SDK
- `examples/` - example addons
>>>>>>> 2cab7e6 (new update)
- `app/src/main/cpp/CMakeLists.txt` - native build
- `game/` - projects (`game/Demo` is included), `game/startup.txt` = project built into the game APK
- `publish.sh` - Termux helper

## License
MIT
