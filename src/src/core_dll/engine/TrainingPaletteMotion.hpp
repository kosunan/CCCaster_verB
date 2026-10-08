#pragma once
#include "core_dll/engine/TrainingPalette.hpp"
#include <cmath>
#include <cstring>
#include <span>

namespace cccaster::training_palette {
struct MotionPoint { float x, y; };
inline std::array<MotionPoint,4> MotionQuad(const Sprite& sprite,const MotionFrame& frame) {
    std::array<MotionPoint,4> points{{{float(sprite.left-128),float(sprite.top-224)},
        {float(sprite.left-128+sprite.width),float(sprite.top-224)},
        {float(sprite.left-128+sprite.width),float(sprite.top-224+sprite.height)},
        {float(sprite.left-128),float(sprite.top-224+sprite.height)}}};
    // 0x407080: 平行移動 → XYZ（またはZXY）回転 → 拡縮。回転値は1周=1。
    constexpr float tau=6.28318530718f;
    const float cx=std::cos(frame.rotationX*tau),sx=std::sin(frame.rotationX*tau);
    const float cy=std::cos(frame.rotationY*tau),sy=std::sin(frame.rotationY*tau);
    const float cz=std::cos(frame.rotationZ*tau),sz=std::sin(frame.rotationZ*tau);
    for(auto& p:points) {
        p.x+=frame.x;p.y+=frame.y;float z=0;
        const auto rx=[&]{const float y=p.y;p.y=y*cx-z*sx;z=y*sx+z*cx;};
        const auto ry=[&]{const float x=p.x;p.x=x*cy+z*sy;z=-x*sy+z*cy;};
        const auto rz=[&]{const float x=p.x;p.x=x*cz-p.y*sz;p.y=x*sz+p.y*cz;};
        if(frame.rotationOrder==0){rx();ry();rz();}
        else if(frame.rotationOrder==1){rz();rx();ry();}
        p.x*=frame.scaleX;p.y*=frame.scaleY;
    }
    return points;
}
inline unsigned MotionFrameIndex(const Motion& motion,unsigned frame) {
    if(motion.frames.empty() || !motion.duration)return NoPixel;
    frame%=motion.duration;
    for(unsigned i=0;i<motion.frames.size();++i) {
        if(frame<motion.frames[i].duration)return i;
        frame-=motion.frames[i].duration;
    }
    return unsigned(motion.frames.size()-1);
}
class MotionPlayer {
    double position=0;
public:
    bool playing=true;
    void Reset(){position=0;playing=true;}
    unsigned Frame() const {return unsigned(position);}
    void Seek(unsigned frame,unsigned duration){position=duration ? frame%duration : 0;}
    void Step(int delta,unsigned duration) {
        playing=false;
        Seek(duration ? unsigned((int64_t(Frame())+duration+delta)%duration) : 0,duration);
    }
    void Advance(double seconds,double speed,unsigned duration) {
        if(!playing || !duration || !std::isfinite(seconds) || seconds<=0 || !std::isfinite(speed) || speed<=0)return;
        position=std::fmod(position+seconds*60*speed,duration);
    }
};
// 読取り専用。ポインター配列と連続配列の双方を扱い、ゲームの更新処理は呼ばない。
// Read(address, destination, size)は全範囲を読み取れた場合だけtrueを返す。
template<class Read> std::vector<Motion> ReadMotions(const Asset& asset,uint32_t table,Read read) {
    const auto item=[&](uint32_t container,unsigned index,size_t required)->uint32_t {
        uint32_t c[4]{};
        if(!container || !read(container,c,sizeof(c)) || c[3]>10000 || index>=c[3] || !c[1])return 0;
        const uint64_t at=uint64_t(c[1])+uint64_t(c[0] ? 4 : c[2])*index;
        if(at>UINT32_MAX || (!c[0] && (c[2]<required || c[2]>4096)))return 0;
        if(!c[0])return uint32_t(at);
        uint32_t result=0;return read(uint32_t(at),&result,4) ? result : 0;
    };
    uint32_t count=0;
    if(!table || table>UINT32_MAX-12 || !read(table+12,&count,4) || count>10000)return {};
    std::vector<Motion> motions;
    size_t totalStates=0;
    for(unsigned id=0;id<count;++id) {
        const auto pattern=item(table,id,0x38);std::array<uint8_t,0x38> p{};
        if(!pattern || !read(pattern,p.data(),p.size()))continue;
        uint32_t states=0,stateCount=0;std::memcpy(&states,p.data()+0x34,4);
        if(!states || states>UINT32_MAX-12 || !read(states+12,&stateCount,4) || stateCount>10000)continue;
        if((totalStates+=stateCount)>200000)break;
        Motion motion;motion.id=id;
        // patternの先頭は32バイト名。ASCII以外は画像名へフォールバックする。
        for(unsigned i=0;i<32 && p[i]>=32 && p[i]<127;++i)motion.name.push_back(char(p[i]));
        bool visible=false;
        for(unsigned state=0;state<stateCount;++state) {
            const auto address=item(states,state,0x54);std::array<uint8_t,0x54> data{};
            if(!address || !read(address,data.data(),data.size())){motion.frames.clear();break;}
            const auto u16=[&](unsigned at){uint16_t v;std::memcpy(&v,data.data()+at,2);return v;};
            MotionFrame frame;frame.state=state;frame.duration=(std::max)(1u,unsigned(u16(0xc)));
            if(u16(0)==0) {
                const auto sprite=std::lower_bound(asset.sprites.begin(),asset.sprites.end(),u16(2),
                    [](const Sprite& value,unsigned id){return value.id<id;});
                if(sprite!=asset.sprites.end() && sprite->id==u16(2))frame.sprite=unsigned(sprite-asset.sprites.begin());
            }
            std::memcpy(&frame.x,data.data()+4,4);std::memcpy(&frame.y,data.data()+8,4);
            std::memcpy(&frame.rotationX,data.data()+0x20,4);std::memcpy(&frame.rotationY,data.data()+0x24,4);
            std::memcpy(&frame.rotationZ,data.data()+0x28,4);frame.rotationOrder=data[0x2c];
            std::memcpy(&frame.scaleX,data.data()+0x30,4);std::memcpy(&frame.scaleY,data.data()+0x34,4);
            bool valid=std::abs(int64_t(frame.x))<=65536 && std::abs(int64_t(frame.y))<=65536;
            for(float f:{frame.scaleX,frame.scaleY,frame.rotationX,frame.rotationY,frame.rotationZ})
                valid=valid && std::isfinite(f) && std::abs(f)<=1024;
            if(!valid)frame.sprite=NoPixel;
            if(frame.sprite!=NoPixel) {
                const auto& sprite=asset.sprites[frame.sprite];
                motion.object=motion.object || sprite.bank || sprite.type==1;
                for(const auto& point:MotionQuad(sprite,frame)) {
                    if(!visible){motion.left=motion.right=point.x;motion.top=motion.bottom=point.y;visible=true;}
                    motion.left=(std::min)(motion.left,point.x);motion.right=(std::max)(motion.right,point.x);
                    motion.top=(std::min)(motion.top,point.y);motion.bottom=(std::max)(motion.bottom,point.y);
                }
                if(motion.name.empty())motion.name=sprite.name;
            } else ++motion.missing;
            motion.duration+=frame.duration;motion.frames.push_back(frame);
        }
        if(visible && !motion.frames.empty())motions.push_back(std::move(motion));
    }
    return motions;
}
}
