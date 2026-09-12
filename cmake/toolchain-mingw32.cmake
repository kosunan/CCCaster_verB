# ============================================================================
# toolchain-mingw32.cmake — Linux から Windows 32bit バイナリを作るための設定
#
# 【何のためにあるか】
#   実機の検証には Windows が要るが、**コンパイルが通るかどうか**の確認は
#   Linux でできる。Windows 環境に触れないまま DLL 側のコードを壊していないか
#   を確かめる用途。生成物を実機で使うことは想定していない（MSYS2 の GCC とは
#   バージョンが違うため、最終的な成果物は必ず MSYS2 mingw32 でビルドすること）。
#
# 【使い方】
#   cmake -B build-win32 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw32.cmake
#   cmake --build build-win32 -j
#
# 【前提】
#   i686-w64-mingw32-* が PATH にあること。Debian/Ubuntu では
#   g++-mingw-w64-i686-posix パッケージ。
#
#   **posix スレッドモデルであること**が必須。win32 スレッドモデルの GCC は
#   std::thread / std::mutex を提供せず、NetplaySession や LogSink がビルドできない。
#   `i686-w64-mingw32-g++-posix --version` で確認する。
# ============================================================================

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(TOOLCHAIN_PREFIX i686-w64-mingw32)

# -posix サフィックス付きを優先し、無ければ素の名前にフォールバックする
find_program(_CC  NAMES ${TOOLCHAIN_PREFIX}-gcc-posix ${TOOLCHAIN_PREFIX}-gcc)
find_program(_CXX NAMES ${TOOLCHAIN_PREFIX}-g++-posix ${TOOLCHAIN_PREFIX}-g++)
find_program(_RC  NAMES ${TOOLCHAIN_PREFIX}-windres)

if(NOT _CC OR NOT _CXX)
    message(FATAL_ERROR "${TOOLCHAIN_PREFIX} のコンパイラが見つかりません。"
                        " Debian/Ubuntu: g++-mingw-w64-i686-posix")
endif()

set(CMAKE_C_COMPILER   ${_CC})
set(CMAKE_CXX_COMPILER ${_CXX})
set(CMAKE_RC_COMPILER  ${_RC})

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})

# ヘッダとライブラリはターゲット側だけを見る。実行ファイルはホスト側から探す。
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
