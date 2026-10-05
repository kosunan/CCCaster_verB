#pragma once
#include "p2p/Protocol.hpp"
#include <stdexcept>
#include <algorithm>

namespace cccaster::gui {
using Json = p2p::Json;
inline std::string String(const Json& value, const char* key, size_t limit) {
    const auto& item = value.at(key);
    if (!item.is_string()) throw std::invalid_argument("string required");
    auto result = item.get<std::string>();
    if (result.size() > limit || result.find('\0') != std::string::npos)
        throw std::invalid_argument("string too long or contains NUL");
    return result;
}
inline int Integer(const Json& value, const char* key, int low, int high) {
    const auto& item = value.at(key);
    if (!item.is_number_integer()) throw std::invalid_argument("integer required");
    const auto result = item.get<int64_t>();
    if (result < low || result > high) throw std::invalid_argument("out of range");
    return static_cast<int>(result);
}
inline bool Boolean(const Json& value, const char* key) {
    if (!value.at(key).is_boolean()) throw std::invalid_argument("boolean required");
    return value.at(key).get<bool>();
}
inline bool CodeAlphabet(const std::string& code) {
    return !code.empty() && std::all_of(code.begin(), code.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-';
    });
}
}
