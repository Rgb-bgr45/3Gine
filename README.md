# 3gine

Tiny stylized low-poly 3D engine for Android, written in C++17 (NativeActivity + EGL + OpenGL ES 3.0). No Java/Kotlin code, no external libraries.

**3D + engine = 3gine.**

## Look
Flat-shaded low-poly meshes, 3-step toon lighting, sky/ground ambient, soft rim light, distance fog, gradient sky, blob shadows, wind-swaying trees, 4x MSAA when the device supports it. No PBR, no realtime shadows, so it runs well on weak phones.

## Controls
- One finger drag: orbit camera
- Two finger pinch: zoom

## Build
Open in Android Studio (NDK 26.1 + CMake 3.22 required), or push to GitHub: the workflow in `.github/workflows/build.yml` builds the APK automatically and uploads it as an artifact. Push a tag like `v0.1` to get it attached to a GitHub Release.

## Layout
- `app/src/main/cpp/main.cpp` - math, meshes, shaders, scene, EGL, input
- `app/src/main/cpp/CMakeLists.txt` - native build

## License
MIT
