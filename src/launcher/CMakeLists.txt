add_library(launcher STATIC
    GameLauncher.cpp
    SpikeDebugger.cpp
)

target_include_directories(launcher PUBLIC 
    ${CMAKE_SOURCE_DIR}/src
)

if(WIN32)
    target_link_options(launcher PRIVATE -static -static-libgcc -static-libstdc++)
endif()
