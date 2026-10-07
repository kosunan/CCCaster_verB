#pragma once
#include "core_dll/engine/TrainingPalette.hpp"
#include <span>

namespace cccaster::training_palette {
constexpr unsigned ExtraCount=6, StandardCount=36, MaxExtraBytes=4*1024*1024;
struct ExtraPart {
    uint32_t component=0, layout=0;
    Edit edit;
};
struct ExtraColor {
    uint32_t character=0, baseColor=0;
    std::vector<ExtraPart> parts;
};
inline bool ReadAct(std::span<const uint8_t> bytes, Palette& colors, std::string& error) {
    if(bytes.size()!=768 && bytes.size()!=772) {error="ACT must contain 768 or 772 bytes.";return false;}
    if(bytes.size()==772) {
        const unsigned count=(unsigned(bytes[768])<<8)|bytes[769];
        const unsigned transparent=(unsigned(bytes[770])<<8)|bytes[771];
        if(count>256 || (transparent!=0 && transparent!=65535)) {
            error="ACT transparency must use index 0 (or none), with up to 256 colors.";return false;
        }
    }
    Palette next{};
    for(unsigned i=0;i<256;++i) next[i]=bytes[i*3]|(uint32_t(bytes[i*3+1])<<8)|(uint32_t(bytes[i*3+2])<<16);
    colors=next;error.clear();return true;
}
inline std::vector<uint8_t> WriteAct(const Palette& colors) {
    std::vector<uint8_t> bytes(772);
    for(unsigned i=0;i<256;++i) for(unsigned c=0;c<3;++c) bytes[i*3+c]=uint8_t(colors[i]>>(c*8));
    bytes[768]=1; return bytes; // big endian: 256 colors, transparent index 0.
}
inline uint32_t ColorHash(std::span<const uint8_t> bytes) {
    uint32_t result=2166136261u;
    for(auto b:bytes) result=(result^b)*16777619u;
    return result ? result : 1;
}
// 読込み側では件数・残量・重複・全消費を検査し、完全なデータだけ公開する。
inline void Put32(std::vector<uint8_t>& bytes,uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes.push_back(uint8_t(value>>(i*8)));
}
inline std::vector<uint8_t> EncodeExtra(const ExtraColor& color) {
    if(color.character>100 || color.baseColor>=36 || color.parts.empty() || color.parts.size()>2)return {};
    std::vector<uint8_t> out;
    unsigned components=0;
    for(auto value:{0x314C4343u,color.character,color.baseColor,uint32_t(color.parts.size())})Put32(out,value);
    for(const auto& part:color.parts) {
        if(part.component>1 || part.edit.effects.size()>1024 || part.edit.replacements.size()>65536 || part.edit.pixels.size()>262144)return {};
        if(components&(1u<<part.component))return {};
        components|=1u<<part.component;
        Put32(out,part.component);Put32(out,part.layout);
        for(auto value:part.edit.palette)Put32(out,value);
        Put32(out,uint32_t(part.edit.effects.size()));
        for(const auto& [bank,values]:part.edit.effects) {
            if(!bank)return {};
            Put32(out,bank);for(auto value:values)Put32(out,value);
        }
        Put32(out,uint32_t(part.edit.replacements.size()));
        for(const auto& [key,value]:part.edit.replacements){if(key>0xffffff || value>0xffffff)return {};Put32(out,key);Put32(out,value);}
        Put32(out,uint32_t(part.edit.pixels.size()));
        for(const auto& [key,value]:part.edit.pixels){if(key>=512u*65536)return {};Put32(out,key);Put32(out,value);}
    }
    if(out.size()+4>MaxExtraBytes)return {};
    Put32(out,ColorHash(out));return out;
}
inline bool DecodeExtra(std::span<const uint8_t> bytes,ExtraColor& out) {
    if(bytes.size()<20 || bytes.size()>MaxExtraBytes)return false;
    size_t at=0;bool ok=true;
    auto get=[&] {uint32_t v=0;if(at+4>bytes.size()){ok=false;return v;}for(unsigned i=0;i<4;++i)v|=uint32_t(bytes[at++])<<(i*8);return v;};
    if(get()!=0x314C4343)return false;
    ExtraColor next;next.character=get();next.baseColor=get();const auto count=get();
    if(next.character>100 || next.baseColor>=36 || !count || count>2)return false;
    unsigned components=0;
    for(unsigned n=0;n<count && ok;++n) {
        ExtraPart part;part.component=get();part.layout=get();
        if(part.component>1 || (components&(1u<<part.component)))return false;
        components|=1u<<part.component;
        for(auto& value:part.edit.palette)value=get();
        const auto banks=get();if(banks>1024 || banks>(bytes.size()-at)/1028)return false;
        for(unsigned i=0;i<banks;++i) {
            const auto id=get();if(!id || part.edit.effects.count(id))return false;
            auto& bank=part.edit.effects[id];for(auto& value:bank)value=get();
        }
        const auto replacements=get();if(replacements>65536 || replacements>(bytes.size()-at)/8)return false;
        for(unsigned i=0;i<replacements;++i) {
            const auto key=get(),value=get();
            if(key>0xffffff || value>0xffffff || !part.edit.replacements.emplace(key,value).second)return false;
        }
        const auto pixels=get();if(pixels>262144 || pixels>(bytes.size()-at)/8)return false;
        for(unsigned i=0;i<pixels;++i) {
            const auto key=get(),value=get();
            if(key>=512u*65536 || !part.edit.pixels.emplace(key,value).second)return false;
        }
        next.parts.push_back(std::move(part));
    }
    if(!ok || at+4!=bytes.size())return false;
    const auto checksum=ColorHash(bytes.first(at));if(get()!=checksum)return false;
    out=std::move(next);return true;
}
}
