#pragma once
#include "core_dll/engine/ExtraColorFile.hpp"
#include <zlib.h>
#include <cstring>

namespace cccaster::training_palette::network {
constexpr uint32_t CompressedColorMagic=0x315A4343;
inline std::vector<uint8_t> CompressColor(std::vector<uint8_t> bytes) {
    if(bytes.empty() || bytes.size()>MaxExtraBytes)return {};
    uLongf size=compressBound(uLong(bytes.size()));
    std::vector<uint8_t> packed(8+size);
    if(compress2(packed.data()+8,&size,bytes.data(),uLong(bytes.size()),Z_DEFAULT_COMPRESSION)!=Z_OK || size+8>=bytes.size())return bytes;
    const uint32_t original=uint32_t(bytes.size());
    std::memcpy(packed.data(),&CompressedColorMagic,4);std::memcpy(packed.data()+4,&original,4);
    packed.resize(size+8);return packed;
}
inline bool DecodeColor(std::span<const uint8_t> bytes,bool compressedAllowed,ExtraColor& out) {
    uint32_t magic=0;
    if(bytes.size()>=4)std::memcpy(&magic,bytes.data(),4);
    if(!compressedAllowed || magic!=CompressedColorMagic)return DecodeExtra(bytes,out);
    if(bytes.size()<9 || bytes.size()>MaxExtraBytes)return false;
    uint32_t expected=0;std::memcpy(&expected,bytes.data()+4,4);
    if(expected<20 || expected>MaxExtraBytes)return false;
    std::vector<uint8_t> raw(expected);uLongf size=expected;uLong consumed=uLong(bytes.size()-8);
    if(uncompress2(raw.data(),&size,bytes.data()+8,&consumed)!=Z_OK || size!=expected || consumed!=bytes.size()-8)return false;
    return DecodeExtra(raw,out);
}
}
