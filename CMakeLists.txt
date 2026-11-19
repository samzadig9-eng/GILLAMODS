cmake_minimum_required(VERSION 3.22)
project(gillamod CXX C)
set(CMAKE_CXX_STANDARD 20)

set(IMGUI third_party/imgui)
add_library(imgui STATIC
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp
  ${IMGUI}/imgui_widgets.cpp ${IMGUI}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${IMGUI} ${IMGUI}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)

set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/dobby dobby_build)

add_library(gillamod SHARED mod.cpp)
target_compile_options(gillamod PRIVATE -O2 -fvisibility=hidden -fno-exceptions -fno-rtti)
target_include_directories(gillamod PRIVATE ${IMGUI} ${IMGUI}/backends)
target_link_libraries(gillamod PRIVATE imgui dobby EGL GLESv3 android log)
