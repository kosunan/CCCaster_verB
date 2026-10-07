#pragma once
#include "core_dll/engine/TrainingPalette.hpp"
#include <span>
#include <limits>

namespace cccaster::training_palette {
// 標準36色の見本に最もよく対応する色番号を特定する。
// 1色だけのRGB逆引きでは重複色や未使用色を誤認するため、全セットで照合する。
inline unsigned RepresentativeIndex(std::span<const Palette> palettes,std::span<const uint32_t> swatches,
                                    const std::array<unsigned,256>& usage) {
    if(palettes.empty() || palettes.size()!=swatches.size())return 1;
    uint64_t best=std::numeric_limits<uint64_t>::max();unsigned selected=1;
    const bool hasBody=std::any_of(usage.begin()+1,usage.end(),[](unsigned count){return count!=0;});
    for(unsigned index=1;index<256;++index) {
        if(hasBody && !usage[index])continue;
        uint64_t distance=0;
        for(unsigned color=0;color<palettes.size();++color)for(unsigned shift:{0u,8u,16u}) {
            const int delta=int(palettes[color][index]>>shift&255)-int(swatches[color]>>shift&255);
            distance+=unsigned(delta*delta);
        }
        if(distance<best || (distance==best && usage[index]>usage[selected])){best=distance;selected=index;}
    }
    return selected;
}
// 代表部分の実画素から最多のRGBを採取。小さな鉛筆変更だけで見本全体は変えない。
inline uint32_t RepresentativeColor(const Edit& edit,unsigned index,std::span<const uint32_t> keys) {
    const uint32_t fallback=edit.palette[index]&0xffffff;
    std::map<uint32_t,unsigned> counts;
    for(const auto key:keys) {
        const auto pixel=edit.pixels.find(key);
        if(pixel!=edit.pixels.end() && (pixel->second>>24)<128)continue;
        ++counts[pixel==edit.pixels.end() ? fallback : pixel->second&0xffffff];
    }
    uint32_t selected=fallback;unsigned best=counts[fallback];
    for(const auto& [color,count]:counts)if(count>best){best=count;selected=color;}
    return selected|0xff000000;
}
}
