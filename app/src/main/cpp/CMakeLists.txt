cmake_minimum_required(VERSION 3.22)
project(threegine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
option(GAME_MODE "Build the player only (no editor UI)" OFF)
include(FetchContent)

# ---- Lua 5.4 ----
FetchContent_Declare(lua_src URL https://github.com/lua/lua/archive/refs/tags/v5.4.7.tar.gz)
FetchContent_GetProperties(lua_src)
if(NOT lua_src_POPULATED)
    FetchContent_Populate(lua_src)
endif()
file(GLOB LUA_SOURCES ${lua_src_SOURCE_DIR}/*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX "/(lua|luac|onelua|ltests)\\.c$")
add_library(lua_lib STATIC ${LUA_SOURCES})
target_compile_definitions(lua_lib PRIVATE LUA_USE_POSIX)
target_include_directories(lua_lib PUBLIC ${lua_src_SOURCE_DIR})

# ---- native_app_glue ----
set(GLUE ${ANDROID_NDK}/sources/android/native_app_glue)
add_library(native_app_glue STATIC ${GLUE}/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${GLUE})

add_library(threegine SHARED main.cpp)
target_link_libraries(threegine lua_lib native_app_glue android EGL GLESv3 log)
set_target_properties(threegine PROPERTIES LINK_FLAGS "-u ANativeActivity_onCreate")

if(GAME_MODE)
    target_compile_definitions(threegine PRIVATE GAME_MODE)
endif()

# ---- Dear ImGui (editor UI, and button/label drawing in the game too) ----
FetchContent_Declare(imgui_src URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.5.tar.gz)
FetchContent_GetProperties(imgui_src)
if(NOT imgui_src_POPULATED)
    FetchContent_Populate(imgui_src)
endif()
set(I ${imgui_src_SOURCE_DIR})
add_library(imgui_lib STATIC
    ${I}/imgui.cpp ${I}/imgui_draw.cpp ${I}/imgui_tables.cpp ${I}/imgui_widgets.cpp
    ${I}/backends/imgui_impl_android.cpp ${I}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui_lib PUBLIC ${I} ${I}/backends)
target_compile_definitions(imgui_lib PUBLIC IMGUI_IMPL_OPENGL_ES3)
target_link_libraries(imgui_lib PUBLIC android EGL GLESv3 log)
target_link_libraries(threegine imgui_lib)

# ---- stb_image (PNG / JPG decoding for the Image UI element) ----
FetchContent_Declare(stb_src URL https://github.com/nothings/stb/archive/refs/heads/master.tar.gz)
FetchContent_GetProperties(stb_src)
if(NOT stb_src_POPULATED)
    FetchContent_Populate(stb_src)
endif()
target_include_directories(threegine PRIVATE ${stb_src_SOURCE_DIR})
