cmake_minimum_required(VERSION 3.10)
project(LabExperiments)

set(CMAKE_CXX_STANDARD 17)

# 1. present_hunter (DLL)
add_library(present_hunter SHARED 
    present_hunter.cpp
    ../../build/_deps/minhook-src/src/buffer.c
    ../../build/_deps/minhook-src/src/hde/hde32.c
    ../../build/_deps/minhook-src/src/hde/hde64.c
    ../../build/_deps/minhook-src/src/hook.c
    ../../build/_deps/minhook-src/src/trampoline.c
)

target_include_directories(present_hunter PRIVATE 
    ../../build/_deps/minhook-src/include
)
target_link_libraries(present_hunter d3d9 winmm)

set_target_properties(present_hunter PROPERTIES 
    OUTPUT_NAME "present_hunter_v24"
    PREFIX ""
)

# 2. injector (EXE)
add_executable(injector injector.cpp)
