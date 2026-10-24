# 3gine

Tiny stylized low-poly 3D engine + on-device editor for Android. C++17 (NativeActivity + EGL + OpenGL ES 3.0), Lua 5.4 scripting, Dear ImGui editor UI. No Java/Kotlin code. Lua and ImGui are downloaded by CMake at build time.

**3D + engine = 3gine.**

## Look
Flat-shaded low-poly meshes, 3-step toon lighting, sky/ground ambient, soft rim light, distance fog, gradient sky, blob shadows, wind-swaying trees, 4x MSAA when supported. No PBR, no realtime shadows, so it runs well on weak phones.

## Two APKs
GitHub Actions builds both from this repo:
- **3gine Editor** - the on-device editor (Unity/Godot style).
- **My 3gine Game** - the exported game: plays the project in `game/`, no editor UI.

## Editor
- Top bar: **Play** (runs your scripts in a sandbox; **Stop** restores the scene exactly as it was), **Save**, **Build APK**, **+Cube / +Cone / +Disc**.
- Left: scene list. Right: inspector (position, size, rotation, colour, spin, sway, blob shadow, duplicate, delete).
- Tap an object in the 3D view to select it. Drag = orbit camera, pinch = zoom.
- Scripts are not editable inside the editor yet (no on-screen keyboard); edit `game/scripts/main.lua` in Termux or on GitHub.

## Project files
The project is the `game/` folder: `scene.lua` (objects, written by the editor) and `scripts/main.lua` (game logic).
The editor saves to `/sdcard/Documents/3gine/game/`. **First run:** Android settings -> Apps -> 3gine Editor -> Permissions -> "All files access". Without it the editor falls back to a private folder that Termux cannot reach.

## Editor -> game APK
1. Press **Save** (or **Build APK**) in the editor.
2. In Termux, in your repo: `sh publish.sh` (copies the project into `game/`, commits, pushes; run `termux-setup-storage` once first).
3. GitHub Actions builds both APKs. Download `3gine-game-apk` from the Actions run (or from Releases if you pushed a `v*` tag).

An APK cannot be compiled on the phone itself (it needs the Android SDK/NDK toolchain), so GitHub does the compiling.
Change the game's app name in `app/build.gradle` (`appLabel` in the `game` flavor).

## Scripting (Lua 5.4)
The engine calls `start()` once and `update(dt)` every frame.

| Function | What it does |
|---|---|
| `engine.spawn{name="Box", mesh="cube"/"cone"/"disc", pos={x,y,z}, scale={x,y,z}, color={r,g,b}, rot=0, spin=0, sway=0, shadow=0}` | creates an object, returns its id |
| `engine.find(name)` | id of the object with that editor name (or nil) |
| `engine.destroy(id)` | removes it |
| `engine.set_pos(id,x,y,z)` / `engine.get_pos(id)` | move / read position |
| `engine.set_scale(id,x,y,z)`, `engine.set_color(id,r,g,b)`, `engine.set_rot(id,r)` / `engine.get_rot(id)` | change an object |
| `engine.camera{yaw=, pitch=, dist=, target={x,y,z}}` | control the camera |
| `engine.set_orbit(false)` | turn off the default touch-orbit camera |
| `engine.touch()` | returns `down, x, y, dx, dy` |
| `engine.screen()` | returns width, height |
| `engine.time()` | seconds since start |
| `engine.load("scripts/other.lua")` | run another script file |
| `engine.log(x)` | print to logcat |

Meshes: `cube` is a unit cube centred on its position; `cone` and `disc` sit on their position (base at y). `shadow` is the radius of a blob shadow on the ground, `sway` bends the top in the wind.

## Layout
- `app/src/main/cpp/main.cpp` - math, meshes, shaders, renderer, Lua bindings, editor UI, EGL, input
- `app/src/main/cpp/CMakeLists.txt` - native build (fetches Lua, and ImGui for the editor flavor)
- `game/` - the project: `scene.lua`, `scripts/main.lua`
- `publish.sh` - Termux helper: editor export -> repo -> push

## License
MIT
