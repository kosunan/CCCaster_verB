#pragma once
#include "shared_contracts/GameBuild.hpp"
#include "shared_contracts/BootDiagnostics.hpp"
#include <limits>

namespace cccaster::boot {
// DLLを実行・ロードせずに公開記述子と初期化RVAを読む。転送exportは受理しない。
class HookDllImage {
    std::span<const uint8_t> bytes;
    size_t table = 0, sections = 0;
    uint32_t exports = 0, exportSize = 0;
    size_t Offset(uint32_t rva, size_t length, bool executable = false) const {
        using namespace game_build;
        size_t found = SIZE_MAX;
        for (size_t i = 0; i < sections; ++i) {
            const size_t s = table + i*40;
            const auto begin = U32(bytes,s+12), rawSize = U32(bytes,s+16), raw = U32(bytes,s+20);
            if (rva < begin || !Fits(rawSize,size_t(rva)-begin,length)) continue;
            if (executable && !(U32(bytes,s+36) & 0x20000000)) return SIZE_MAX;
            const uint64_t offset = uint64_t(raw)+rva-begin;
            if (offset > SIZE_MAX || !Fits(bytes.size(),size_t(offset),length) || found != SIZE_MAX) return SIZE_MAX;
            found = size_t(offset);
        }
        return found;
    }
    uint32_t Export(std::string_view name) const {
        using namespace game_build;
        const auto e = Offset(exports,40);
        if (e == SIZE_MAX) return 0;
        const auto count = U32(bytes,e+24), functions = U32(bytes,e+20);
        if (count > 4096 || functions > 4096) return 0;
        const auto names = Offset(U32(bytes,e+32),count*4), ordinals = Offset(U32(bytes,e+36),count*2);
        const auto addresses = Offset(U32(bytes,e+28),functions*4);
        if (names == SIZE_MAX || ordinals == SIZE_MAX || addresses == SIZE_MAX) return 0;
        for (size_t i = 0; i < count; ++i) {
            const auto n = Offset(U32(bytes,names+i*4),name.size()+1);
            if (n == SIZE_MAX || std::memcmp(bytes.data()+n,name.data(),name.size()) || bytes[n+name.size()]) continue;
            const auto ordinal = U16(bytes,ordinals+i*2);
            if (ordinal >= functions) return 0;
            const auto rva = U32(bytes,addresses+ordinal*4);
            if (rva >= exports && uint64_t(rva) < uint64_t(exports)+exportSize) return 0;
            return rva;
        }
        return 0;
    }
public:
    Descriptor descriptor{};
    uint32_t initializeRva = 0, imageSize = 0;
    bool Inspect(std::span<const uint8_t> input) {
        using namespace game_build;
        bytes = input; descriptor = {}; initializeRva = 0;
        PeIdentity identity;
        if (!ReadHeaders(bytes,identity) || identity.machine != 0x14c) return false;
        const auto nt = U32(bytes,0x3c), opt = nt+24;
        if (!(U16(bytes,nt+22)&0x2000) || U16(bytes,nt+20)<104 || U32(bytes,opt+92)<1) return false;
        imageSize = identity.imageSize;
        sections = U16(bytes,nt+6); table = opt+U16(bytes,nt+20);
        exports = U32(bytes,opt+96); exportSize = U32(bytes,opt+100);
        if (!exports || exportSize<40 || !Fits(imageSize,exports,exportSize)) return false;
        const auto info = Export("CCCasterStartupInfo");
        const auto offset = Offset(info,sizeof(Descriptor));
        initializeRva = Export("CCCasterInitialize");
        if (!info || offset == SIZE_MAX || !initializeRva || initializeRva >= imageSize ||
            Offset(initializeRva,1,true) == SIZE_MAX) return false;
        std::memcpy(&descriptor,bytes.data()+offset,sizeof(descriptor));
        return true;
    }
};
}
