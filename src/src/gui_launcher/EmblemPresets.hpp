#pragma once
#include "EmblemCatalog.hpp"
#include "shared_contracts/EmblemBitmap.hpp"
#include <windows.h>

namespace cccaster::emblem {
inline bool ImportPreset(std::string_view code, Image& output) {
    if (!FindPreset(code)) return false;
    auto name = "EMBLEM_" + std::string(code);
    for (auto& c : name) if (c >= 'a' && c <= 'z') c += 'A' - 'a';
    const auto module = GetModuleHandleW(nullptr);
    const auto resource = FindResourceA(module, name.c_str(), MAKEINTRESOURCEA(10));
    if (!resource) return false;
    const auto memory = LoadResource(module, resource);
    const auto* data = memory ? static_cast<const uint8_t*>(LockResource(memory)) : nullptr;
    return data && DecodeBitmap({data, SizeofResource(module, resource)}, output);
}
}
