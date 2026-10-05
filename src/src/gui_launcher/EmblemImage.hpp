#pragma once
#include "shared_contracts/EmblemFile.hpp"

namespace cccaster::emblem {
inline bool Import(const std::filesystem::path& path, Image& output) {
    auto extension = path.extension().wstring();
    for (auto& c : extension) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    return extension == L".bmp" && Load(path, output);
}
}
