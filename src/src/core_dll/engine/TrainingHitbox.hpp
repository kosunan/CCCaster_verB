#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

namespace cccaster::training_hitbox {
enum class Kind : unsigned { Attack, Hurt, Push, Shield, Clash, Throw, Count };
inline constexpr unsigned Count=unsigned(Kind::Count);
inline constexpr std::array<const char*,Count> Names{"ATTACK","HURT","PUSH","SHIELD","CLASH","THROW (CONTACT)"};
struct Rect { int32_t x0=0,y0=0,x1=0,y1=0; bool operator==(const Rect&) const = default; };
struct Pose {
    int32_t x=0,y=0,offsetX=0,offsetY=0,cameraX=0,cameraY=0;
    bool left=false,attached=false,doubleSize=false;
};
// 0x419630のADD/SUB/NEG/SARと同じ32bit演算。生矩形は符号付き16bit。
inline int32_t Add(int32_t a,int32_t b){return std::bit_cast<int32_t>(uint32_t(a)+uint32_t(b));}
inline int32_t Neg(int32_t a){return std::bit_cast<int32_t>(0u-uint32_t(a));}
inline Rect Transform(Rect r,const Pose& p) {
    if(p.left){r.x0=Neg(r.x0);r.x1=Neg(r.x1);}
    if(p.doubleSize){r.x0=Add(r.x0,r.x0);r.y0=Add(r.y0,r.y0);r.x1=Add(r.x1,r.x1);r.y1=Add(r.y1,r.y1);}
    const auto x=Add(Add(Add(p.x,p.attached?p.offsetX:0),Neg(p.cameraX))>>7,320);
    const auto y=Add(Add(Add(p.y,p.attached?p.offsetY:0),Neg(p.cameraY))>>7,432);
    r.x0=Add(r.x0,x);r.x1=Add(r.x1,x);r.y0=Add(r.y0,y);r.y1=Add(r.y1,y);
    if(r.x0>r.x1)std::swap(r.x0,r.x1);
    if(r.y0>r.y1)std::swap(r.y0,r.y1);
    return r;
}
// 0x46ECA0: 0、0x46F530: 1..8、0x468060: 9、0x46F2F0: 11。
// 未同定のslot10や12以降へ意味を付けない。
inline Kind DefenseKind(unsigned slot) {
    return slot==0?Kind::Push:slot<=8?Kind::Hurt:slot==9?Kind::Shield:slot==11?Kind::Clash:Kind::Count;
}
struct Options {
    std::array<bool,Count> visible{};
    unsigned selected=0;
    bool Any()const {for(bool v:visible)if(v)return true;return false;}
    void Move(int delta){selected=(selected+Count+delta)%Count;}
    void Set(bool value){visible[selected]=value;}
    void Toggle(){visible[selected]=!visible[selected];}
    unsigned Mask()const {unsigned mask=0;for(unsigned i=0;i<Count;++i)if(visible[i])mask|=1u<<i;return mask;}
};
}
