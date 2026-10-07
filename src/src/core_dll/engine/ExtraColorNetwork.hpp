#pragma once
#include "core_dll/engine/ExtraColorFile.hpp"
#include <memory>
#include <mutex>
#include <cstring>

namespace cccaster::training_palette::network {
// 通信版10/拡張8の任意末尾。標準の選択色番号・入力・同期領域は変更しない。
constexpr uint32_t Magic=0x31435058, ChunkBytes=384, MaxBytes=MaxExtraBytes+12;
struct Blob {
    uint32_t epoch=0,serial=0,hash=0;
    std::vector<uint8_t> bytes;
    unsigned Character() const {uint32_t v=UINT32_MAX;if(bytes.size()>=8)std::memcpy(&v,bytes.data(),4);return v;}
    unsigned Extra() const {uint32_t v=UINT32_MAX;if(bytes.size()>=8)std::memcpy(&v,bytes.data()+4,4);return v;}
    unsigned Revision() const {uint32_t v=0;if(bytes.size()>=12)std::memcpy(&v,bytes.data()+8,4);return v;}
};
inline bool ValidBytes(std::span<const uint8_t> bytes) {
    if(bytes.size()<12 || bytes.size()>MaxBytes)return false;
    uint32_t character,extra;std::memcpy(&character,bytes.data(),4);std::memcpy(&extra,bytes.data()+4,4);
    if(extra==UINT32_MAX)return bytes.size()==12 && (character<=100 || character==UINT32_MAX);
    ExtraColor color;
    return extra<6 && DecodeExtra(bytes.subspan(12),color) && color.character==character;
}
inline std::shared_ptr<const Blob> Make(uint32_t epoch,uint32_t serial,uint32_t character,uint32_t extra,
                                      const ExtraColor* color=nullptr,uint32_t revision=0) {
    Blob value;value.epoch=epoch;value.serial=serial;
    Put32(value.bytes,character);Put32(value.bytes,extra);Put32(value.bytes,revision);
    if(color){auto bytes=EncodeExtra(*color);value.bytes.insert(value.bytes.end(),bytes.begin(),bytes.end());}
    if(!ValidBytes(value.bytes))return {};
    value.hash=ColorHash(value.bytes);return std::make_shared<const Blob>(std::move(value));
}
struct Chunk {
    uint32_t magic=Magic,epoch=0,serial=0,hash=0,size=0,index=0;
    uint32_t ackEpoch=0,ackSerial=0,ackHash=0,wanted=0;
    std::array<uint8_t,ChunkBytes> bytes{};
    bool Valid() const {
        return magic==Magic && (!epoch || (epoch%65536==0)) && serial && hash && size>=12 && size<=MaxBytes &&
               index<(size+ChunkBytes-1)/ChunkBytes && wanted<=MaxBytes/ChunkBytes+1;
    }
};
class Receiver {
    Blob pending;
    std::vector<bool> received;
    std::shared_ptr<const Blob> complete,cached;
public:
    uint32_t Epoch() const {return pending.epoch;}
    uint32_t Serial() const {return pending.serial;}
    unsigned Wanted() const {for(unsigned i=0;i<received.size();++i)if(!received[i])return i;return 0;}
    std::shared_ptr<const Blob> Get() const {return complete;}
    std::shared_ptr<const Blob> Accept(const Chunk& chunk) {
        if(!chunk.Valid() || chunk.epoch<pending.epoch || (chunk.epoch==pending.epoch && chunk.serial<pending.serial))return {};
        if(chunk.epoch!=pending.epoch || chunk.serial!=pending.serial) {
            if(complete)cached=complete;
            pending={chunk.epoch,chunk.serial,chunk.hash,std::vector<uint8_t>(chunk.size)};
            received.assign((chunk.size+ChunkBytes-1)/ChunkBytes,false);complete.reset();
        }
        if(chunk.hash!=pending.hash || chunk.size!=pending.bytes.size() || complete)return {};
        // 再戦・確定revision更新は画像が同じ。先頭チャンクと受信済み全体を
        // 組み合わせ、全体hashと形式が一致した場合だけ再受信を省略する。
        if(!chunk.index && cached && cached->bytes.size()==pending.bytes.size()) {
            auto reused=cached->bytes;
            std::memcpy(reused.data(),chunk.bytes.data(),(std::min)(size_t(ChunkBytes),reused.size()));
            if(ColorHash(reused)==pending.hash && ValidBytes(reused)) {
                pending.bytes=std::move(reused);std::fill(received.begin(),received.end(),true);
                complete=std::make_shared<const Blob>(pending);return complete;
            }
        }
        const auto offset=chunk.index*ChunkBytes;
        std::memcpy(pending.bytes.data()+offset,chunk.bytes.data(),(std::min)(size_t(ChunkBytes),pending.bytes.size()-offset));
        received[chunk.index]=true;
        if(std::find(received.begin(),received.end(),false)!=received.end())return {};
        if(ColorHash(pending.bytes)!=pending.hash || !ValidBytes(pending.bytes)) {
            std::fill(received.begin(),received.end(),false);return {};
        }
        complete=std::make_shared<const Blob>(pending);return complete;
    }
};
class Store {
    inline static std::mutex mutex;
    inline static std::shared_ptr<const Blob> local,peer;
    inline static bool supported=false,acknowledged=false;
public:
    static void Reset() {std::lock_guard lock(mutex);local=Make(0,1,UINT32_MAX,UINT32_MAX);peer.reset();supported=acknowledged=false;}
    static void Local(std::shared_ptr<const Blob> value) {std::lock_guard lock(mutex);local=std::move(value);acknowledged=false;}
    static std::shared_ptr<const Blob> Local() {std::lock_guard lock(mutex);return local;}
    static std::shared_ptr<const Blob> Peer() {std::lock_guard lock(mutex);return peer;}
    static void Received(const Chunk& chunk,std::shared_ptr<const Blob> value) {
        std::lock_guard lock(mutex);supported=true;if(value)peer=std::move(value);
        if(local && chunk.ackEpoch==local->epoch && chunk.ackSerial==local->serial && chunk.ackHash==local->hash)acknowledged=true;
    }
    static bool Supported() {std::lock_guard lock(mutex);return supported;}
    static bool Acknowledged(){std::lock_guard lock(mutex);return acknowledged;}
    static bool Ready(uint32_t epoch,uint32_t localCharacter,uint32_t peerCharacter,uint32_t localRevision,uint32_t peerRevision) {
        std::lock_guard lock(mutex);
        if(!supported)return !local || local->Extra()==UINT32_MAX;
        return local && peer && acknowledged && local->epoch==epoch && peer->epoch==epoch &&
               local->Character()==localCharacter && peer->Character()==peerCharacter &&
               local->Revision()==localRevision && peer->Revision()==peerRevision;
    }
};
class Exchange {
    Receiver receiver;
    unsigned sequence=0,wanted=0;
    uint32_t targetEpoch=0,targetSerial=0;
    int64_t lastSent=-1;
public:
    void Reset(){*this={};Store::Reset();}
    void Receive(const Chunk& chunk) {
        if(!chunk.Valid())return;
        const auto local=Store::Local();
        if(local && chunk.ackEpoch==local->epoch && chunk.ackSerial==local->serial)wanted=chunk.wanted;
        Store::Received(chunk,receiver.Accept(chunk));
    }
    bool Next(Chunk& chunk,int64_t nowUs) {
        const auto local=Store::Local();if(!local)return false;
        if(targetEpoch!=local->epoch || targetSerial!=local->serial){targetEpoch=local->epoch;targetSerial=local->serial;sequence=wanted=0;lastSent=-1;}
        if(Store::Acknowledged() && receiver.Get() && lastSent>=0 && nowUs-lastSent<1000000)return false;
        lastSent=nowUs;
        chunk={};chunk.epoch=local->epoch;chunk.serial=local->serial;chunk.hash=local->hash;chunk.size=uint32_t(local->bytes.size());
        const auto count=(chunk.size+ChunkBytes-1)/ChunkBytes;
        chunk.index=(wanted+(sequence++%8))%count;
        const auto offset=chunk.index*ChunkBytes;
        std::memcpy(chunk.bytes.data(),local->bytes.data()+offset,(std::min)(size_t(ChunkBytes),local->bytes.size()-offset));
        chunk.ackEpoch=receiver.Epoch();chunk.ackSerial=receiver.Serial();
        if(auto received=receiver.Get())chunk.ackHash=received->hash;
        chunk.wanted=receiver.Wanted();return true;
    }
};
}
