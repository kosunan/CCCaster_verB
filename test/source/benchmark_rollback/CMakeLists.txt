cmake_minimum_required(VERSION 3.14)
project(RollbackBenchmark LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SSE2 を有効化（32bit MinGW では明示指定が必要な場合がある）
if(MSVC)
    add_compile_options(/O2 /arch:SSE2)
else()
    add_compile_options(-O2 -msse2)
endif()

add_executable(RollbackBenchmark
    benchmark_main.cpp
)

# ヘッダーのみのライブラリなのでインクルードパスのみ
target_include_directories(RollbackBenchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
