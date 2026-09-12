add_executable(CCCaster_v10 
    main.cpp
    ConfigManager.cpp
    controller/MainController.cpp
    ui/ConsoleRenderer.cpp
    network_wrapper/SessionNegotiator.cpp
    network_wrapper/ConnectionHash.cpp
)

# Root include directory and ASIO
target_include_directories(CCCaster_v10 PRIVATE 
    ${CMAKE_SOURCE_DIR}/src
    ${asio_SOURCE_DIR}/asio/include
)

# Link local libraries and system APIs
target_link_libraries(CCCaster_v10 PRIVATE launcher)

if(WIN32)
    target_link_libraries(CCCaster_v10 PRIVATE ws2_32 wsock32 wininet winmm)
endif()

# Compiler flags
if(MSVC)
    target_compile_options(CCCaster_v10 PRIVATE /W4 /WX-)
    if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
        target_compile_definitions(CCCaster_v10 PRIVATE DO_DEBUG_LOGGING)
    endif()
    # MSVCでの静的リンク (MT/MTd)
    set_property(TARGET CCCaster_v10 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(CCCaster_v10 PRIVATE -Wall -Wextra)
    
    # 実行ファイルに必要なDLL（libstdc++, libgcc等）をすべて静的リンクしてポータブルにする
    target_link_options(CCCaster_v10 PRIVATE -static -static-libgcc -static-libstdc++)
    
    if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
        target_compile_definitions(CCCaster_v10 PRIVATE DO_DEBUG_LOGGING)
    endif()
endif()

target_link_libraries(CCCaster_v10 PRIVATE cccaster_transport)
