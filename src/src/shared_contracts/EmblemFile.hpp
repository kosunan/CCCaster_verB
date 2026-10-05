#pragma once
#include "shared_contracts/EmblemBitmap.hpp"
#include <filesystem>
#include <fstream>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace cccaster::emblem {
inline constexpr const char* FileName = "player-emblem.bmp";
inline bool Load(const std::filesystem::path& path, Image& result) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < std::streamoff(BitmapBytes) || input.tellg() > 1024 * 1024) return false;
    std::vector<uint8_t> data(static_cast<size_t>(input.tellg()));
    input.seekg(0); input.read(reinterpret_cast<char*>(data.data()), data.size());
    return input && DecodeBitmap(data, result);
}
inline bool Save(const std::filesystem::path& path, const Image& image) {
    if (!Valid(image)) return false;
    if (!image.id) { std::error_code error; std::filesystem::remove(path, error); return !error; }
    // BMPには透過情報がない。保存前後で画像IDが変わるデータは拒否する。
    for (unsigned i = 3; i < Bytes; i += 4) if (image.pixels[i] != 255) return false;
    const auto data = EncodeBitmap(image);
    auto temp = path;
#ifdef _WIN32
    temp += ".tmp." + std::to_string(GetCurrentProcessId());
#else
    temp += ".tmp";
#endif
    bool written = false;
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), data.size());
        out.flush(); written = bool(out); out.close(); written = written && !out.fail();
    }
    bool saved = false;
    if (written) {
#ifdef _WIN32
        saved = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
        std::error_code error; std::filesystem::rename(temp, path, error); saved = !error;
#endif
    }
    if (!saved) { std::error_code ignored; std::filesystem::remove(temp, ignored); }
    return saved;
}
}
