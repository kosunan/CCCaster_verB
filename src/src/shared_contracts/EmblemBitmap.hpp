#pragma once
#include "shared_contracts/PlayerEmblem.hpp"
#include <span>

namespace cccaster::emblem {
inline constexpr unsigned BitmapPixels = Width * Height * 3, BitmapBytes = 54 + BitmapPixels;
// little endianを明示し、非圧縮24bitのWindows BMPだけを受理する。
inline uint32_t BmpUint(std::span<const uint8_t> data, size_t at, unsigned bytes = 4) {
    uint32_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= uint32_t(data[at + i]) << (i * 8);
    return value;
}
inline bool DecodeBitmap(std::span<const uint8_t> data, Image& output) {
    if (data.size() < BitmapBytes || data.size() > 1024 * 1024 || data[0] != 'B' || data[1] != 'M') return false;
    const auto header = BmpUint(data, 14), offset = BmpUint(data, 10), height = BmpUint(data, 22);
    if ((header != 40 && header != 108 && header != 124) || BmpUint(data, 2) != data.size() ||
        BmpUint(data, 18) != Width || (height != Height && height != uint32_t(-int(Height))) ||
        BmpUint(data, 26, 2) != 1 || BmpUint(data, 28, 2) != 24 || BmpUint(data, 30) != 0 ||
        (BmpUint(data, 34) != 0 && BmpUint(data, 34) != BitmapPixels) ||
        offset < 14 + header || offset > data.size() - BitmapPixels) return false;
    Image draft;
    for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
        const auto sourceY = height == Height ? Height - y - 1 : y;
        const auto source = offset + (sourceY * Width + x) * 3, dest = (y * Width + x) * 4;
        std::memcpy(draft.pixels.data() + dest, data.data() + source, 3);
        draft.pixels[dest + 3] = 255;
    }
    draft.id = Hash(draft.pixels); output = draft; return true;
}
inline std::array<uint8_t, BitmapBytes> EncodeBitmap(const Image& image) {
    std::array<uint8_t, BitmapBytes> data{};
    const auto put = [&](unsigned at, uint32_t value, unsigned bytes = 4) {
        for (unsigned i = 0; i < bytes; ++i) data[at + i] = uint8_t(value >> (i * 8));
    };
    data[0] = 'B'; data[1] = 'M';
    put(2, BitmapBytes); put(10, 54); put(14, 40); put(18, Width); put(22, Height);
    put(26, 1, 2); put(28, 24, 2); put(34, BitmapPixels);
    for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x)
        std::memcpy(data.data() + 54 + ((Height - y - 1) * Width + x) * 3,
                    image.pixels.data() + (y * Width + x) * 4, 3);
    return data;
}
}
