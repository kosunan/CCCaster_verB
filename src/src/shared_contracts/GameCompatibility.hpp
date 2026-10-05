#pragma once
#include "shared_contracts/GameBuild.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

namespace cccaster::game_compat {
// ファイルの同一性と、固定アドレスを使用できる契約を分離する。
// 署名外のゲームロジックや対戦同期の完全な同一性を保証するものではない。
inline constexpr uint32_t Base = 0x400000;
inline constexpr uint32_t Read = 0x40000000, Write = 0x80000000, Execute = 0x20000000;
struct Section { uint32_t rva, size, raw, rawSize, flags; };
struct Image {
    uint32_t entry = 0, size = 0;
    size_t count = 0;
    std::array<Section,96> sections{};
    bool Parse(std::span<const uint8_t> bytes, bool file) {
        using namespace game_build;
        *this = {};
        if (bytes.size()<64 || U16(bytes,0)!=0x5a4d) return false;
        const size_t nt=U32(bytes,60);
        if (nt<64 || !Fits(bytes.size(),nt,24) || U32(bytes,nt)!=0x4550 || U16(bytes,nt+4)!=0x14c ||
            !(U16(bytes,nt+22)&2) || (U16(bytes,nt+22)&0x2000)) return false;
        const size_t opt=nt+24, optSize=U16(bytes,nt+20), table=opt+optSize;
        count=U16(bytes,nt+6);
        if (optSize<96 || !Fits(bytes.size(),opt,optSize) || U16(bytes,opt)!=0x10b ||
            U32(bytes,opt+28)!=Base || !count || count>sections.size() ||
            !Fits(bytes.size(),table,count*40)) return false;
        entry=U32(bytes,opt+16); size=U32(bytes,opt+56);
        const auto headers=U32(bytes,opt+60);
        // 起動前・ロード後の両方で同じヘッダー表を読める範囲に限定する。
        if (size>64*1024*1024 || headers<table+count*40 || table+count*40>4096 || headers>size) return false;
        for(size_t i=0;i<count;++i) {
            const size_t s=table+i*40;
            sections[i]={U32(bytes,s+12),std::max(U32(bytes,s+8),U32(bytes,s+16)),
                U32(bytes,s+20),U32(bytes,s+16),U32(bytes,s+36)};
            const auto &v=sections[i];
            if (!v.size || v.rva<headers || !Fits(size,v.rva,v.size) ||
                (v.rawSize && (v.raw<headers || (file && !Fits(bytes.size(),v.raw,v.rawSize))))) return false;
            for(size_t j=0;j<i;++j) {
                const auto &p=sections[j];
                if (uint64_t(v.rva)<uint64_t(p.rva)+p.size && uint64_t(p.rva)<uint64_t(v.rva)+v.size) return false;
                if (v.rawSize && p.rawSize && uint64_t(v.raw)<uint64_t(p.raw)+p.rawSize &&
                    uint64_t(p.raw)<uint64_t(v.raw)+v.rawSize) return false;
            }
        }
        return Covers(entry,2,Read|Execute) && Offset(entry,2)!=SIZE_MAX;
    }
    bool Covers(uint32_t rva,size_t length,uint32_t flags) const {
        if (!length || !game_build::Fits(size,rva,length)) return false;
        uint64_t cursor=rva, end=cursor+length;
        while(cursor<end) {
            const Section *found=nullptr;
            for(size_t i=0;i<count;++i) {
                const auto &s=sections[i];
                if (cursor>=s.rva && cursor<uint64_t(s.rva)+s.size) { found=&s; break; }
            }
            if (!found || (found->flags&flags)!=flags) return false;
            cursor=std::min(end,uint64_t(found->rva)+found->size);
        }
        return true;
    }
    size_t Offset(uint32_t rva,size_t length) const {
        for(size_t i=0;i<count;++i) {
            const auto &s=sections[i];
            if (rva>=s.rva && game_build::Fits(s.rawSize,rva-s.rva,length))
                return size_t(s.raw)+rva-s.rva;
        }
        return SIZE_MAX;
    }
};

struct Signature { const char *name; uint32_t rva; std::string_view hex; };
// 両実体で一致を確認。入力基点/stride、player stride、時計/モード、戦闘停止、RNGの参照を含む。
// 言語文字列・リソース・初期キー設定・PE timestamp/checksumは契約に含めない。
inline constexpr Signature Required[]{
    {"input_pointer",0x1e61c,"8b0dace676006bc02c8dbc0818050000"},
    {"input_write_1",0x1f098,"890689460489460889460c"},
    {"input_write_2",0xa0230,
        "568bf085f60f8487000000833f0075108b460485c075078bcee832fcffff890733c08bcee827fcffff894704"
        "b8010000008bcee818fcffff894708b8020000008d48feba01000000d3e2855624740309570c8d48ffba01000000"
        "d3e2855624740309570cba010000008bc8d3e2855624740309570c8d4801ba01000000d3e2855624740309570c"
        "83c0048d48fe83f9207cae833d68e7760000752a83fb027d25833f008d349b8d3475c0d25400750c8bc6e85afeffff"
        "0fb6d089178bcee8cefeffff09470c5ec3"},
    {"player_layout",0x1c1d8,"80b83051550000750433c0eb068d80345155000fb680e400000083f801741983f803741481c6fc0a000081fe148055007c96"},
    {"world_timer",0x337bf,"d91d205155008305d4d15500018bc65e59c3"},
    {"game_mode",0x33803,"a1d0d155003905e8ee5400743e83f864a3e8ee54007534"},
    {"battle_state",0x23630,"833d98d5740000c60576355600017405e83b8e0200803d03d2550000752e833d482a5600007525"},
    {"battle_freeze",0x23998,"392d482a5600892d00df55007e0c291d482a5600891d00df5500"},
    {"rng_state",0x21a80,"8b0d6840560083c10183f9387c05b90100000083f922890d684056008d51157e038d51de8b048d6c4056002b04956c405600790505ffffff7f83057c3756000189048d6c405600a378375600c3"}
};
inline uint8_t Hex(char value) { return value<='9' ? value-'0' : value-'a'+10; }
enum class Issue { None, Image, DataLayout, CodeRange, ReadFailure, Signature };
struct Result {
    Issue issue=Issue::None;
    const char *name="";
    uint32_t address=0;
    explicit operator bool() const { return issue==Issue::None; }
};
inline const char *Name(Issue issue) {
    constexpr const char *names[]{"none","image","data_layout","code_range","read","signature"};
    return uint32_t(issue)<std::size(names) ? names[uint32_t(issue)] : "unknown";
}
template<class Reader> Result Check(const Image &image,Reader read) {
    if (!image.Covers(0x1000,0x11918f,Read|Execute)) return {Issue::CodeRange,"game_code",0x401000};
    if (!image.Covers(0x11b000,0x2f5ee,Read)) return {Issue::DataLayout,"game_constants",0x51b000};
    // 保存表と固定データ参照が存在する元ゲームの静的データ領域。
    if (!image.Covers(0x14b000,0x266d64,Read|Write)) return {Issue::DataLayout,"game_data",0x54b000};
    for(const auto &s:Required) {
        const size_t length=s.hex.size()/2;
        if (!image.Covers(s.rva,length,Read|Execute) || image.Offset(s.rva,length)==SIZE_MAX)
            return {Issue::CodeRange,s.name,Base+s.rva};
        std::array<uint8_t,256> bytes{};
        if (length>bytes.size() || !read(s.rva,std::span(bytes).first(length)))
            return {Issue::ReadFailure,s.name,Base+s.rva};
        for(size_t i=0;i<length;++i)
            if (bytes[i]!=uint8_t(Hex(s.hex[i*2])*16+Hex(s.hex[i*2+1])))
                return {Issue::Signature,s.name,Base+s.rva+uint32_t(i)};
    }
    return {};
}
inline Result Inspect(std::span<const uint8_t> bytes) {
    Image image;
    if (!image.Parse(bytes,true)) return {Issue::Image,"pe32",0};
    return Check(image,[&](uint32_t rva,std::span<uint8_t> out) {
        const auto offset=image.Offset(rva,out.size());
        if (offset==SIZE_MAX || !game_build::Fits(bytes.size(),offset,out.size())) return false;
        std::memcpy(out.data(),bytes.data()+offset,out.size()); return true;
    });
}
}
