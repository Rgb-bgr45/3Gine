# 3gine

Ultra-light stylized low-poly 3D engine + on-device editor for Android.
C++17 (NativeActivity, EGL, OpenGL ES 3.0), Lua 5.4 scripting, Dear ImGui editor. Lua, ImGui and stb_image are downloaded by CMake at build time. MIT license.

**3D + engine = 3gine.** Two APKs are built from this repo by GitHub Actions:
- **3gine Editor** - project menu, scene editor, play-test sandbox.
- **Game APK** - plays the project named in `game/startup.txt`, no editor UI.

## Build (GitHub)
Extract this zip into the root of your repo, then `git add -A && git commit -m "3gine" && git push`.
Open the **Actions** tab: artifacts `3gine-editor-apk` and `3gine-game-apk`. A tag like `v0.1` also attaches both APKs to a Release.
Local build: Android Studio / `gradle assembleEditorDebug assembleGameDebug` (NDK 26.1.10909125, CMake 3.22.1).

## First run
1. **Welcome** screen (once): choose a language (English default) -> OK. Saved in `<app files>/setting/config.json`.
2. Grant **All files access** (Android settings -> Apps -> 3gine Editor -> Permissions) so projects can live in `/sdcard/Documents/3gine/`.
3. Project menu: **Create** (asks for a name) / **Open** / Refresh / Import .3ne / Depot. Projects: `Documents/3gine/game/<name>/`.

## Editor
Top bar: Menu | Play (sandbox, Stop restores the scene) | **Prepare for APK** | Save | + Add | Scenes | Snap | More (Mods, Depot, Backup .3ne).
- **+ Add**: Cube, Cone, Disc, Cylinder, Sphere, Pyramid, Prism, Wedge, Torus, Stairs, **Camera**, **Light**, UI: Button, Label, Joystick, Image.
- Tap an object to select it. A **move gizmo** (X red, Y green, Z blue) appears: drag an arrow, with **Snap** Off / 0.5 / 1.0. Drag = orbit, pinch = zoom.
- Inspector: name, folder, hide, lock, parent, position/size/rotation, colour, material, light intensity/range, camera look-at + FOV, script (New / Pick / Edit / Clear).
- Scene list is grouped by **folder**; children are indented under their **parent** (`world = parentWorld x local`). **Hide** = editor only, **Lock** = cannot be picked/edited.
- **Scenes**: switch / create (`scenes/<name>.lua`; the first scene is `scene.lua` = "main"). In Lua: `engine.load_scene("Level2")`.
- Script editor (top half of the screen, the on-screen keyboard works, plus a symbol bar). Scripts are `scripts/<name>.lua`.
- Rendering starts **Unlit** (cheap built-in look). The first Light object or custom shader switches to **Forward** automatically (shown in the top bar).

## Lua API
Scripts attached to an object get `self` (its id): `start(self)`, `update(self, dt)`, and `on_click(self)` for buttons. Optional global `scripts/main.lua` (`start()`, `update(dt)`), and a global `on_collision(a, b)`.

| Function | |
|---|---|
| `spawn{name,mesh,pos,scale,color,rot,spin,sway,shadow,text,script,material,parent,folder,...}` | create, returns id. mesh: cube cone disc cylinder sphere pyramid prism wedge torus stairs camera light button label joystick image |
| `find(name)`, `find_all(name)`, `destroy(id)`, `get_name(id)` | |
| `get_pos / set_pos / move`, `get_world_pos`, `get_scale / set_scale`, `get_rot / set_rot`, `set_color` | |
| `set_parent(id,parentId)`, `get_parent(id)` | hierarchy |
| `get_distance(a,b)`, `move_towards(id,x,y,z,speed)`, `check_raycast(x,y,z,dx,dy,dz)` -> id, distance, `get_bounds(id)` | |
| `set_camera_look_at(cam,x,y,z)` or `(cam,targetId)`, `set_camera_fov(cam,fov)`, `camera{...}`, `set_orbit(false)` | the first Camera object is the view while playing |
| `set_light_properties(id,r,g,b,intensity,range)`, `set_material(id,"metal")`, `load_shader("file.frag")` | materials: matte plastic glossy metal glow rubber, or a depot `name.mat` |
| `set_text / get_text`, `set_image(id,"file.png")` | UI |
| `load_scene(name)` | |
| `play_sound("file.mp3", loop)`, `stop_sounds()` | sounds from the depot / project |
| `touch()`, `screen()`, `time()`, `log(x)`, `load(file)` | globals `joystick_x`, `joystick_y` (-1..1) come from a Joystick element |
| `physics_add_body(id,{shape="box"/"sphere",mass,restitution,static})` (needs 3sics enabled), `physics_set_gravity`, `physics_add_impulse`, `physics_set_velocity`, `physics_get_velocity`, `physics_remove_body`, `physics_clear` | **3sics** physics |

UI elements: `pos`/`scale` are % of the screen (Joystick: `scale.x` = diameter in % of screen height). The editor uses a system font (Roboto) so Turkish, German, Russian text works.

## 3sics physics (built-in addon)
Gravity, AABB + sphere collisions, restitution (the smaller of the two bodies' values), mass and impulses. It is C++ inside the engine (`threesics.hpp`), not a Lua file: it appears in **More > Mods** as `3sics (built-in, C++)` with an **Enable / Disable** button (the switch is saved in the app's settings folder). Tick **Required** and Play refuses to start when it is off. The game APK always includes it. Use unparented objects; fine for dozens of bodies (pairs are checked one by one); bodies do not rotate.

## Addons (mods)
**More > Mods** lists the built-in 3sics and every addon found in `Documents/3gine/addons/`. Enable/Disable on a file addon adds/removes `.disable` on its name (if Android refuses the rename it copies the file under the new name and shows the reason in the status line). Disabled files are ignored at start.
- `.lua` addons return `{ start=function() end, update=function(dt) end }` (`examples/addons/spin_all.lua`).
- `.so` addons are native (arm64): export `gine_addon_init(const GineAPI*)` (`gine_addon.h`, `examples/addons/native_example.cpp`).
- `.dll` / `.gpp` are PC formats: listed as unsupported.
- **Required** writes the addon to `project.json`: Play refuses to start when it is missing or disabled and says which one (the game APK shows the same message instead of crashing).
- Lua errors and native crashes go to `Documents/3gine/crash.log` with the name of the mod that was running.

## Custom shaders
`<project>/shaders/name.frag`, `#version 300 es` (see `examples/shaders/rainbow.frag`); `engine.load_shader("name.frag")`. Provided: `vN`, `vW`, `uColor`, `uCam`, `uSun`, `uFog`, `uTime`, `uAlpha`, `uMat`, `uForward`, `uLN`, `uLP[4]`, `uLC[4]`, `uLR[4]`.

## Depot (assets)
`<app files>/img`, `/sound`, `/material` (`Android/data/com.threegine.engine/files/`). Add files with a file manager, or in the editor: **More > Depot > + Add file** (built-in file browser copies the file). png/jpg, mp3/ogg/wav, and `.mat` materials (`specular= shininess= emissive= metal=`). On Android 11+ file managers may be blocked from `Android/data`; the in-app picker always works.

## Backup, APK
- **Backup .3ne** (More menu): everything - scenes, scripts, shaders, sounds, images - in one archive (a standard zip, stored uncompressed) at `Download/3Gine-project/<name>/<name>.3ne`. Import it from the project menu.
- **Prepare for APK** (next to Play, text depends on the language): writes `Download/3Gine-project/<name>/<name>.zip` with the engine source + your project, ready to build. An APK cannot be compiled on the phone, so upload it to GitHub (extract first) and Actions builds `3gine-game-apk`. Or run `sh publish.sh` in Termux to push `Documents/3gine/game` into this repo's `game/`.
- Change the package name (e.g. `dev.3gine.ge`) in `app/build.gradle` (`applicationId`, `namespace`); asset/config paths follow the package.

## Layout
`app/src/main/cpp/main.cpp` engine + editor, `threesics.hpp` physics, `depot.hpp` asset depot, `gine_addon.h` native addon SDK, `CMakeLists.txt`; `game/` projects (`Demo` included: scenes, UI, joystick, camera, light, physics) and `startup.txt`; `examples/`; `publish.sh`; `.github/workflows/build.yml`.
