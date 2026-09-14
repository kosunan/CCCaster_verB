#pragma once
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace cccaster::public_api {

constexpr std::size_t PlayerNameSize = 32;

// MBAAの既定フォントで確実に描ける表示名へ正規化する。
// 制御文字・不正なマルチバイト列をHUDへ渡さず、空名には役割名を使う。
inline void NormalizePlayerName(char *output, std::size_t outputSize, const char *input,
                                const char *fallback) {
    if (!output || outputSize == 0)
        return;
    std::memset(output, 0, outputSize);
    std::size_t written = 0;
    if (input) {
        while (*input && written + 1 < outputSize) {
            const unsigned char ch = static_cast<unsigned char>(*input++);
            if (ch >= 0x20 && ch <= 0x7e)
                output[written++] = static_cast<char>(ch);
        }
    }
    while (written && output[written - 1] == ' ')
        output[--written] = '\0';
    std::size_t first = 0;
    while (output[first] == ' ')
        ++first;
    if (first)
        std::memmove(output, output + first, written - first + 1);
    if (!output[0] && fallback)
        std::snprintf(output, outputSize, "%s", fallback);
}

template <std::size_t N>
inline void NormalizePlayerName(char (&output)[N], const char *input, const char *fallback) {
    NormalizePlayerName(output, N, input, fallback);
}

} // namespace cccaster::public_api
