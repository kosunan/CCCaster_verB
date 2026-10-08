#pragma once
#include "core_dll/engine/ExtraColorCompression.hpp"
#include <memory>
#include <mutex>
#include <cstring>
#include <deque>

namespace cccaster::training_palette::network {
// 通信版10/拡張8の任意末尾。標準の選択色番号・入力・同期領域は変更しない。
constexpr uint32_t Magic=0x31435058, ChunkBytes=384, MaxBytes=MaxExtraBytes+12;
constexpr uint32_t FastMagic=0x32435058, FastCapability=0x31504643;
constexpr uint32_t ReceivePolicyCapability=0x31504352;
inline uint32_t TransferHash(std::span<const uint8_t> bytes,bool fast) {
    if(!fast)return ColorHash(bytes);
    uint32_t result=2166136261u;
    // 色の内容と選択revisionを分離。同じ色なら確定操作で転送を破棄しない。
    for(size_t i=0;i<bytes.size();++i)result=(result^((i>=8 && i<12) ? 0 : bytes[i]))*16777619u;
    return result ? result : 1;
}
struct Blob {
    uint32_t epoch=0,serial=0,hash=0;
    std::vector<uint8_t> bytes;
    bool fast=false;
    unsigned Character() const {uint32_t v=UINT32_MAX;if(bytes.size()>=8)std::memcpy(&v,bytes.data(),4);return v;}
    unsigned Extra() const {uint32_t v=UINT32_MAX;if(bytes.size()>=8)std::memcpy(&v,bytes.data()+4,4);return v;}
    unsigned Revision() const {uint32_t v=0;if(bytes.size()>=12)std::memcpy(&v,bytes.data()+8,4);return v;}
};
inline bool ValidBytes(std::span<const uint8_t> bytes,bool fast=false) {
    if(bytes.size()<12 || bytes.size()>MaxBytes)return false;
    uint32_t character,extra;std::memcpy(&character,bytes.data(),4);std::memcpy(&extra,bytes.data()+4,4);
    if(extra==UINT32_MAX)return bytes.size()==12 && (character<=100 || character==UINT32_MAX);
    ExtraColor color;
    return extra<6 && DecodeColor(bytes.subspan(12),fast,color) && color.character==character;
}
inline std::shared_ptr<const Blob> Make(uint32_t epoch,uint32_t serial,uint32_t character,uint32_t extra,
                                      const ExtraColor* color=nullptr,uint32_t revision=0,bool fast=false) {
    Blob value;value.epoch=epoch;value.serial=serial;value.fast=fast;
    Put32(value.bytes,character);Put32(value.bytes,extra);Put32(value.bytes,revision);
    if(color){auto bytes=EncodeExtra(*color);if(fast)bytes=CompressColor(std::move(bytes));value.bytes.insert(value.bytes.end(),bytes.begin(),bytes.end());}
    if(!ValidBytes(value.bytes,fast))return {};
    value.hash=TransferHash(value.bytes,fast);return std::make_shared<const Blob>(std::move(value));
}
inline std::shared_ptr<const Blob> Rebind(const Blob& previous,uint32_t epoch,uint32_t serial,uint32_t revision) {
    auto value=previous;value.epoch=epoch;value.serial=serial;
    std::memcpy(value.bytes.data()+8,&revision,4);
    if(!value.fast)value.hash=TransferHash(value.bytes,false);
    return std::make_shared<const Blob>(std::move(value));
}
struct Chunk {
    uint32_t magic=Magic,epoch=0,serial=0,hash=0,size=0,index=0;
    uint32_t ackEpoch=0,ackSerial=0,ackHash=0,wanted=0;
    std::array<uint8_t,ChunkBytes> bytes{};
    bool Valid() const {
        return (magic==Magic || magic==FastMagic) && (!epoch || (epoch%65536==0)) && serial && hash && size>=12 && size<=MaxBytes &&
               index<(size+ChunkBytes-1)/ChunkBytes && wanted<=MaxBytes/ChunkBytes+1;
    }
};
class Receiver {
    Blob pending;
    bool acceptsColors=true;
    std::vector<bool> received;
    std::shared_ptr<const Blob> complete;
    std::deque<std::shared_ptr<const Blob>> cached;
    void Cache() {
        if(!complete || complete->Extra()>=6)return;
        std::erase_if(cached,[&](const auto& item){return item->hash==complete->hash && item->fast==complete->fast;});
        cached.push_front(complete);
        size_t bytes=0;
        for(const auto& item:cached)bytes+=item->bytes.size();
        while(cached.size()>4 || bytes>16u*1024*1024){bytes-=cached.back()->bytes.size();cached.pop_back();}
    }
public:
    Receiver()=default;
    explicit Receiver(bool accepts):acceptsColors(accepts){}
    uint32_t Epoch() const {return pending.epoch;}
    uint32_t Serial() const {return pending.serial;}
    unsigned Wanted() const {for(unsigned i=0;i<received.size();++i)if(!received[i])return i;return 0;}
    std::shared_ptr<const Blob> Get() const {return complete;}
    std::shared_ptr<const Blob> Accept(const Chunk& chunk) {
        if(!chunk.Valid() || chunk.epoch<pending.epoch || (chunk.epoch==pending.epoch && chunk.serial<pending.serial))return {};
        if(!acceptsColors && chunk.size>12) {
            // 受信拒否に未対応の相手には先頭の選択情報だけをACKする。
            // カラー本体の確保・組立て・展開・キャッシュは行わない。
            if(chunk.index)return {};
            uint32_t character,extra,revision;
            std::memcpy(&character,chunk.bytes.data(),4);std::memcpy(&extra,chunk.bytes.data()+4,4);
            std::memcpy(&revision,chunk.bytes.data()+8,4);
            if(character>100 || extra>=6)return {};
            if(complete && chunk.epoch==pending.epoch && chunk.serial==pending.serial)return {};
            pending={chunk.epoch,chunk.serial,chunk.hash,{},chunk.magic==FastMagic};
            Put32(pending.bytes,character);Put32(pending.bytes,UINT32_MAX);Put32(pending.bytes,revision);
            received.clear();cached.clear();complete=std::make_shared<const Blob>(pending);return complete;
        }
        if(chunk.epoch!=pending.epoch || chunk.serial!=pending.serial) {
            Cache();
            const bool resume=chunk.magic==FastMagic && pending.fast && chunk.hash==pending.hash && chunk.size==pending.bytes.size();
            if(!resume) {
                pending={chunk.epoch,chunk.serial,chunk.hash,std::vector<uint8_t>(chunk.size),chunk.magic==FastMagic};
                received.assign((chunk.size+ChunkBytes-1)/ChunkBytes,false);
            } else {
                pending.epoch=chunk.epoch;pending.serial=chunk.serial;
                received[0]=false; // 新revisionを含む先頭だけは必ず受け取る。
            }
            complete.reset();
        }
        if(chunk.hash!=pending.hash || (chunk.magic==FastMagic)!=pending.fast || chunk.size!=pending.bytes.size() || complete)return {};
        // 再戦・確定revision更新は画像が同じ。先頭チャンクと受信済み全体を
        // 組み合わせ、全体hashと形式が一致した場合だけ再受信を省略する。
        if(!chunk.index)for(const auto& item:cached)if(item->fast==pending.fast && item->bytes.size()==pending.bytes.size()) {
            if(pending.fast && item->hash!=pending.hash)continue;
            auto reused=item->bytes;
            std::memcpy(reused.data(),chunk.bytes.data(),(std::min)(size_t(ChunkBytes),reused.size()));
            if(TransferHash(reused,pending.fast)==pending.hash && ValidBytes(reused,pending.fast)) {
                pending.bytes=std::move(reused);std::fill(received.begin(),received.end(),true);
                complete=std::make_shared<const Blob>(pending);return complete;
            }
        }
        const auto offset=chunk.index*ChunkBytes;
        std::memcpy(pending.bytes.data()+offset,chunk.bytes.data(),(std::min)(size_t(ChunkBytes),pending.bytes.size()-offset));
        received[chunk.index]=true;
        if(std::find(received.begin(),received.end(),false)!=received.end())return {};
        if(TransferHash(pending.bytes,pending.fast)!=pending.hash || !ValidBytes(pending.bytes,pending.fast)) {
            std::fill(received.begin(),received.end(),false);return {};
        }
        complete=std::make_shared<const Blob>(pending);return complete;
    }
};
class Store {
    inline static std::mutex mutex;
    inline static std::shared_ptr<const Blob> local,peer;
    inline static bool supported=false,fastSupported=false,acknowledged=false;
    inline static bool acceptsColors=true,peerAcceptsColors=true,policyAcknowledged=false;
public:
    static void Reset(bool accepts=true) {std::lock_guard lock(mutex);local=Make(0,1,UINT32_MAX,UINT32_MAX);peer.reset();supported=fastSupported=acknowledged=policyAcknowledged=false;acceptsColors=accepts;peerAcceptsColors=true;}
    static void Local(std::shared_ptr<const Blob> value) {std::lock_guard lock(mutex);local=std::move(value);acknowledged=false;}
    static std::shared_ptr<const Blob> Local() {std::lock_guard lock(mutex);return local;}
    static std::shared_ptr<const Blob> Peer() {std::lock_guard lock(mutex);return acceptsColors ? peer : nullptr;}
    static void Received(const Chunk& chunk,std::shared_ptr<const Blob> value) {
        std::lock_guard lock(mutex);supported=true;if(value)peer=std::move(value);
        uint32_t capability=0;std::memcpy(&capability,chunk.bytes.data()+ChunkBytes-4,4);
        // 12バイトの標準色通知の未使用末尾で対応を宣言。旧版も通常通知として読める。
        if(chunk.magic==FastMagic || (chunk.size==12 && capability==FastCapability))fastSupported=true;
        uint32_t policy=0,allowed=1;std::memcpy(&policy,chunk.bytes.data()+ChunkBytes-12,4);std::memcpy(&allowed,chunk.bytes.data()+ChunkBytes-8,4);
        if(chunk.size==12 && policy==ReceivePolicyCapability && allowed<=1)peerAcceptsColors=allowed!=0;
        if(local && chunk.ackEpoch==local->epoch && chunk.ackSerial==local->serial && chunk.ackHash==local->hash) {
            acknowledged=true;
            if(local->epoch==0 && local->serial==1)policyAcknowledged=true;
        }
    }
    static bool Supported() {std::lock_guard lock(mutex);return supported;}
    static bool FastSupported() {std::lock_guard lock(mutex);return fastSupported;}
    static bool Acknowledged(){std::lock_guard lock(mutex);return acknowledged;}
    static bool AcceptsColors(){std::lock_guard lock(mutex);return acceptsColors;}
    static bool PeerAcceptsColors(){std::lock_guard lock(mutex);return peerAcceptsColors;}
    static bool PolicyAcknowledged(){std::lock_guard lock(mutex);return policyAcknowledged;}
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
    uint32_t sentAckEpoch=0,sentAckSerial=0,sentAckHash=0,sentWanted=0;
    int64_t lastSent=-1;
public:
    void Reset(bool accepts=true){*this={};receiver=Receiver(accepts);Store::Reset(accepts);}
    void Receive(const Chunk& chunk) {
        if(!chunk.Valid())return;
        const auto local=Store::Local();
        if(local && chunk.ackEpoch==local->epoch && chunk.ackSerial==local->serial && chunk.wanted<(local->bytes.size()+ChunkBytes-1)/ChunkBytes)
            wanted=chunk.wanted;
        Store::Received(chunk,receiver.Accept(chunk));
    }
    bool Next(Chunk& chunk,int64_t nowUs) {
        const auto local=Store::Local();if(!local)return false;
        if(targetEpoch!=local->epoch || targetSerial!=local->serial){targetEpoch=local->epoch;targetSerial=local->serial;sequence=wanted=0;lastSent=-1;}
        const auto peer=receiver.Get();const uint32_t ackHash=peer ? peer->hash : 0;
        const bool ackChanged=sentAckEpoch!=receiver.Epoch() || sentAckSerial!=receiver.Serial() || sentAckHash!=ackHash || sentWanted!=receiver.Wanted();
        if(Store::Acknowledged() && peer && !ackChanged && lastSent>=0 && nowUs-lastSent<1000000)return false;
        lastSent=nowUs;
        chunk={};chunk.magic=local->fast ? FastMagic : Magic;chunk.epoch=local->epoch;chunk.serial=local->serial;chunk.hash=local->hash;chunk.size=uint32_t(local->bytes.size());
        const auto count=(chunk.size+ChunkBytes-1)/ChunkBytes;
        chunk.index=(wanted+(sequence++%(local->fast ? 32 : 8)))%count;
        const auto offset=chunk.index*ChunkBytes;
        std::memcpy(chunk.bytes.data(),local->bytes.data()+offset,(std::min)(size_t(ChunkBytes),local->bytes.size()-offset));
        chunk.ackEpoch=receiver.Epoch();chunk.ackSerial=receiver.Serial();
        chunk.ackHash=ackHash;chunk.wanted=receiver.Wanted();
        if(chunk.size==12) {
            const uint32_t allowed=Store::AcceptsColors();
            std::memcpy(chunk.bytes.data()+ChunkBytes-12,&ReceivePolicyCapability,4);
            std::memcpy(chunk.bytes.data()+ChunkBytes-8,&allowed,4);
            std::memcpy(chunk.bytes.data()+ChunkBytes-4,&FastCapability,4);
        }
        sentAckEpoch=chunk.ackEpoch;sentAckSerial=chunk.ackSerial;sentAckHash=chunk.ackHash;sentWanted=chunk.wanted;
        return true;
    }
};
}
