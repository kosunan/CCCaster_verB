cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" CCCASTER_VERSION LIMIT_COUNT 1)
project(CCCaster_verB VERSION ${CCCASTER_VERSION} LANGUAGES C CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../VERSION")
configure_file(cmake/ProductVersion.hpp.in generated/ProductVersion.hpp @ONLY)
include_directories("${CMAKE_CURRENT_BINARY_DIR}/generated")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build configurations
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# VERSIONを変えない開発ビルド同士も識別する。生成物・ゲーム・設定は入力に含めない。
file(GLOB_RECURSE CCCASTER_ID_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gui_launcher/ui/*" "${CMAKE_CURRENT_SOURCE_DIR}/src/gui_launcher/*.rc.in" "${CMAKE_CURRENT_SOURCE_DIR}/src/gui_launcher/assets/app.manifest"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/GRP/*.dds"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/*")
list(APPEND CCCASTER_ID_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
list(SORT CCCASTER_ID_INPUTS)
set(CCCASTER_ID_TEXT "${CCCASTER_VERSION}|${CMAKE_CXX_COMPILER_ID}|${CMAKE_CXX_COMPILER_VERSION}|${CMAKE_BUILD_TYPE}|${CMAKE_CXX_FLAGS}|${CMAKE_CXX_FLAGS_RELEASE}")
foreach(input IN LISTS CCCASTER_ID_INPUTS)
    file(SHA256 "${input}" digest)
    file(RELATIVE_PATH relative "${CMAKE_CURRENT_SOURCE_DIR}" "${input}")
    string(APPEND CCCASTER_ID_TEXT "\n${relative}:${digest}")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CCCASTER_ID_INPUTS})
string(SHA256 CCCASTER_BUILD_ID "${CCCASTER_ID_TEXT}")
configure_file(cmake/BuildIdentity.hpp.in generated/BuildIdentity.hpp @ONLY)

# ==========================================
# 成果物出力先の一元化設定 (exe, dllなどをすべてこのディレクトリ直下に集約)
# ==========================================
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
include(cmake/StageSelectAssets.cmake)

foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_BINARY_DIR}/lib)
endforeach()

message("Building CCCaster in ${CMAKE_BUILD_TYPE} mode")

# ==========================================
# Dependencies Setup
# ==========================================
include(FetchContent)

# ==========================================
# ターゲット環境の検証
# ==========================================
# 注入先の MBAA.exe は 32bit。64bit の DLL を作っても注入は「無言で」失敗し、
# エラーも警告も出ないまま何時間も溶ける。ビルド時に落とす。
if(WIN32 AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "32bit ツールチェインが必要です（現在: ${CMAKE_SIZEOF_VOID_P}バイトポインタ）。\n"
        "  MSYS2:  PATH に C:\\msys64\\mingw32\\bin を先頭で通してから cmake を実行\n"
        "  Linux:  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw32.cmake\n"
        "64bit でも DLL は生成できますが、32bit の MBAA.exe には注入できません。")
endif()

# 対象 Windows バージョンを明示する。
# これまでツールチェイン既定値に任せていたため、MSYS2(GCC15, 既定 Win10) では通り、
# 古い mingw-w64(既定 WinXP SP2) では inet_pton / inet_ntop が未宣言でビルドが落ちた。
# どのツールチェインでも同じ API 集合が見えるように固定する。
if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0601 WINVER=0x0601)   # Windows 7
endif()

# Fetch ASIO (Used by App Layer)
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-30-2
)
FetchContent_MakeAvailable(asio)

# MinHook / ImGui は DLL 層だけが使い、どちらも Windows 専用
# （MinHook は x86 命令長デコーダ、ImGui バックエンドは win32/dx9）。
# 非 Windows では取得も定義もしない。
if(WIN32)

# Fetch MinHook (Used by DLL Layer)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.3
    SOURCE_SUBDIR  cccaster-manual-target
)
FetchContent_MakeAvailable(minhook)

add_library(minhook STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde32.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)

# Fetch ImGui (Used by DLL Layer)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.4-docking
    SOURCE_SUBDIR  cccaster-manual-target
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_options(imgui PRIVATE -static -static-libgcc -static-libstdc++)

endif() # WIN32 — MinHook / ImGui

# 通信実装は実行系ごとに重複したソース一覧を持たない。
add_library(cccaster_transport STATIC
    src/core_dll/network/UdpSocket.cpp
    src/core_dll/network/NetworkSimulator.cpp
)
target_compile_features(cccaster_transport PUBLIC cxx_std_20)
target_include_directories(cccaster_transport PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(cccaster_transport SYSTEM PRIVATE ${asio_SOURCE_DIR}/asio/include)
if(WIN32)
    target_link_libraries(cccaster_transport PUBLIC ws2_32 mswsock)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(cccaster_transport PUBLIC Threads::Threads)
endif()

# ==========================================
# Submodules
# ==========================================
enable_testing()

if(WIN32)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
    FetchContent_MakeAvailable(nlohmann_json)
    add_library(cccaster_p2p_protocol STATIC src/p2p/Crypto.cpp src/p2p/Protocol.cpp)
    target_include_directories(cccaster_p2p_protocol PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(cccaster_p2p_protocol PUBLIC nlohmann_json::nlohmann_json bcrypt crypt32 ws2_32)
    target_link_libraries(cccaster_transport PUBLIC cccaster_p2p_protocol)
    add_library(cccaster_p2p STATIC src/p2p/Ntfy.cpp src/p2p/Session.cpp src/p2p/Watch.cpp
        src/p2p/Matching.cpp src/p2p/MatchingProtocol.cpp)
    target_include_directories(cccaster_p2p PRIVATE ${asio_SOURCE_DIR}/asio/include)
    target_link_libraries(cccaster_p2p PUBLIC cccaster_transport winhttp iphlpapi)
    if(MSVC)
        set_property(TARGET cccaster_p2p_protocol cccaster_p2p PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    # 実機用の成果物。DLL 注入と D3D9/DirectInput/MinHook を含むため Windows 専用。
    add_subdirectory(src/launcher)
    add_subdirectory(src/core_dll)
    add_subdirectory(src/cli_launcher)
    add_subdirectory(src/gui_launcher)
else()
    message(STATUS "非 Windows 環境: harness と単体テストのみビルドする")
endif()

# 両OS共通。harness はゲーム無しで同期ロジックを通しで走らせる実行ファイル。
add_subdirectory(src/harness)
add_subdirectory(src/tests)
