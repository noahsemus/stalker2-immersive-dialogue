cmake_minimum_required(VERSION 3.22)
set(TARGET ImmersiveDialogue)
project(${TARGET})

add_library(${TARGET} SHARED "dllmain.cpp")
target_include_directories(${TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
# Link the UE4SS SDK target (provided by the RE-UE4SS subdirectory).
target_link_libraries(${TARGET} PUBLIC UE4SS)
# Win32 raw input / user32 for the mouse hook, xinput for gamepad polling.
target_link_libraries(${TARGET} PUBLIC user32 xinput)
set_target_properties(${TARGET} PROPERTIES OUTPUT_NAME "main")
