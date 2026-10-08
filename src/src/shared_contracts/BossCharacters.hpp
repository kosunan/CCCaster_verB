#pragma once
#include <array>
#include <cstdint>

namespace cccaster::boss {
inline constexpr std::array<uint32_t,8> Characters{16,32,53,58,59,72,73,85};
// 標準9列のRANDOM（49）の左右4セル。0x48B430/0x427F30/0x427F80と照合。
inline constexpr std::array<unsigned,8> Cells{45,46,47,48,50,51,52,53};
inline constexpr unsigned CellCount=54;
constexpr int Index(uint32_t character) {
    for(unsigned i=0;i<Characters.size();++i)if(Characters[i]==character)return int(i);
    return -1;
}
constexpr bool IsBoss(uint32_t character){return Index(character)>=0;}
constexpr uint32_t Moon(uint32_t character){return character==32 ? 0 : character==53 ? 8 : 9;}
constexpr int Cell(uint32_t character){return IsBoss(character) ? int(Cells[Index(character)]) : -1;}
constexpr uint32_t Base(uint32_t character) {
    return character==16 ? 3 : character>=50 && IsBoss(character) ? character-50 : character;
}
constexpr bool Enabled(unsigned mode,bool local,bool peer) {
    return mode==1 ? local : mode==0 && local && peer;
}
}
