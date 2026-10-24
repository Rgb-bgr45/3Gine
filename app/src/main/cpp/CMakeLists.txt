cmake_minimum_required(VERSION 3.22)
project(threegine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(GLUE ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(native_app_glue STATIC ${GLUE}/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${GLUE})

add_library(threegine SHARED main.cpp)
target_link_libraries(threegine native_app_glue android EGL GLESv3 log)
set_target_properties(threegine PROPERTIES LINK_FLAGS "-u ANativeActivity_onCreate")
