#include "StartupAssets.hpp"
#include <d3dx9tex.h>
#include <bcrypt.h>
#include <array>
#include <vector>
#include <string>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <memory>
#include "core_dll/mbaa_mem/StartupPatch.hpp"
#include "core_dll/common/StartupTrace.hpp"
#include "core_dll/common/DataPaths.hpp"

namespace cccaster::game_memory::startup_assets {
namespace {
using CreateFn = decltype(&D3DXCreateTextureFromFileInMemoryEx);
using SaveFn = decltype(&D3DXSaveTextureToFileInMemory);
using Digest = std::array<uint8_t,32>;
bool active=false, verify=false, firstFast=false;
BCRYPT_ALG_HANDLE sha=nullptr;
SaveFn save=nullptr;
std::string directory;
Digest adapterHash{};
bool adapterReady=false;
unsigned hits=0, misses=0, saved=0, verified=0, rejected=0, bypass=0;
unsigned direct=0, directVerified=0, directRejected=0;
unsigned legacyEqual=0, legacyDifferent=0;
uint64_t written=0;
constexpr uint32_t Limit=64*1024*1024;
struct Header {
    uint32_t magic=0x31414343, version=2, bytes=0;
    D3DXIMAGE_INFO info{};
    uint32_t width=0,height=0,format=0;
    Digest digest{};
};
bool Supported(D3DFORMAT format) {
    return format==D3DFMT_A8R8G8B8 || format==D3DFMT_X8R8G8B8 ||
        format==D3DFMT_DXT1 || format==D3DFMT_DXT2 || format==D3DFMT_DXT3 ||
        format==D3DFMT_DXT4 || format==D3DFMT_DXT5;
}
bool Hash(const void* first, size_t size, const void* second, size_t extra, Digest& out) {
    BCRYPT_HASH_HANDLE hash=nullptr;
    if(!sha || size>UINT32_MAX || extra>UINT32_MAX || BCryptCreateHash(sha,&hash,nullptr,0,nullptr,0,0)<0) return false;
    bool ok=BCryptHashData(hash,(PUCHAR)first,static_cast<ULONG>(size),0)>=0 &&
        (!extra || BCryptHashData(hash,(PUCHAR)second,static_cast<ULONG>(extra),0)>=0) &&
        BCryptFinishHash(hash,out.data(),out.size(),0)>=0;
    BCryptDestroyHash(hash);
    return ok;
}
bool Adapter(IDirect3DDevice9* device) {
    if(adapterReady) return true;
    IDirect3D9* d3d=nullptr;
    D3DDEVICE_CREATION_PARAMETERS params{};
    D3DADAPTER_IDENTIFIER9 id{};
    D3DCAPS9 caps{};
    if(FAILED(device->GetCreationParameters(&params)) || FAILED(device->GetDeviceCaps(&caps)) ||
        FAILED(device->GetDirect3D(&d3d))) return false;
    auto result=d3d->GetAdapterIdentifier(params.AdapterOrdinal,0,&id);
    d3d->Release();
    adapterReady=SUCCEEDED(result) && Hash(&id,sizeof(id),&caps,sizeof(caps),adapterHash);
    return adapterReady;
}
std::string Key(const void* data, UINT size) {
    Digest digest{};
    if(!Hash(data,size,adapterHash.data(),adapterHash.size(),digest)) return {};
    std::string key;
    constexpr char hex[]="0123456789abcdef";
    for(auto value:digest) {key+=hex[value>>4];key+=hex[value&15];}
    return directory+"/"+key+".dds-cache";
}
bool Read(const std::string& path, Header& header, std::vector<uint8_t>& bytes) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file) return false;
    const auto length=file.tellg();
    file.seekg(0);
    if(!file.read(reinterpret_cast<char*>(&header),sizeof(header)) || header.magic!=0x31414343 || header.version!=2 ||
        header.bytes<128 || header.bytes>Limit || length!=std::streamoff(sizeof(header)+header.bytes) ||
        !header.width || !header.height || header.width>16384 || header.height>16384 ||
        !Supported(static_cast<D3DFORMAT>(header.format))) return false;
    bytes.resize(header.bytes);
    Digest digest{};
    return bool(file.read(reinterpret_cast<char*>(bytes.data()),bytes.size())) &&
        Hash(bytes.data(),bytes.size(),&header,offsetof(Header,digest),digest) && digest==header.digest;
}
bool Equal(IDirect3DTexture9* a, IDirect3DTexture9* b) {
    D3DSURFACE_DESC x{},y{};
    if(FAILED(a->GetLevelDesc(0,&x)) || FAILED(b->GetLevelDesc(0,&y)) ||
        x.Width!=y.Width || x.Height!=y.Height || x.Format!=y.Format ||
        !Supported(x.Format) || a->GetLevelCount()!=b->GetLevelCount()) return false;
    D3DLOCKED_RECT p{},q{};
    if(FAILED(a->LockRect(0,&p,nullptr,D3DLOCK_READONLY))) return false;
    if(FAILED(b->LockRect(0,&q,nullptr,D3DLOCK_READONLY))) {a->UnlockRect(0);return false;}
    const bool compressed=x.Format!=D3DFMT_A8R8G8B8 && x.Format!=D3DFMT_X8R8G8B8;
    const UINT rowBytes=compressed?((x.Width+3)/4)*(x.Format==D3DFMT_DXT1?8:16):x.Width*4;
    const UINT rows=compressed?(x.Height+3)/4:x.Height;
    bool same=p.Pitch>=int(rowBytes) && q.Pitch>=int(rowBytes);
    for(UINT row=0;same && row<rows;++row)
        same=std::memcmp(static_cast<char*>(p.pBits)+row*p.Pitch,static_cast<char*>(q.pBits)+row*q.Pitch,rowBytes)==0;
    auto qa=b->UnlockRect(0),pa=a->UnlockRect(0);
    return same && SUCCEEDED(qa) && SUCCEEDED(pa);
}
void Write(const std::string& path, D3DXIMAGE_INFO info, IDirect3DTexture9* texture) {
    D3DSURFACE_DESC desc{};
    if(FAILED(texture->GetLevelDesc(0,&desc)) || texture->GetLevelCount()!=1 ||
        !Supported(desc.Format) || written>=256ull*1024*1024) return;
    ID3DXBuffer* buffer=nullptr;
    if(FAILED(save(&buffer,D3DXIFF_DDS,texture,nullptr)) || !buffer) return;
    auto release=[](ID3DXBuffer* p){p->Release();};
    std::unique_ptr<ID3DXBuffer,decltype(release)> owner(buffer,release);
    Header header{};
    header.info=info;header.width=desc.Width;header.height=desc.Height;header.format=desc.Format;header.bytes=buffer->GetBufferSize();
    if(header.bytes>=128 && header.bytes<=Limit && Hash(buffer->GetBufferPointer(),header.bytes,&header,offsetof(Header,digest),header.digest)) {
        const auto temp=path+"."+std::to_string(GetCurrentProcessId())+".tmp";
        bool ok=false;
        {
            std::ofstream file(temp,std::ios::binary|std::ios::trunc);
            file.write(reinterpret_cast<const char*>(&header),sizeof(header));
            file.write(static_cast<const char*>(buffer->GetBufferPointer()),header.bytes);
            file.close();ok=bool(file);
        }
        if(ok && MoveFileExA(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING)) {++saved;written+=header.bytes;}
        else DeleteFileA(temp.c_str());
    }
}
bool DirectDds(IDirect3DDevice9* device, const void* data, UINT size,
    D3DXIMAGE_INFO* info, IDirect3DTexture9** texture, HRESULT& result) {
    if(!firstFast || size<128 || std::memcmp(data,"DDS ",4)) return false;
    uint32_t header[32]{};
    std::memcpy(header,data,128);
    // 対象ゲームの一段DXT画像。未知のヘッダー・矩形・パレットは通常APIに戻す。
    const UINT h=header[3],w=header[4];
    const auto format=static_cast<D3DFORMAT>(header[21]);
    if(header[1]!=124 || header[2]!=4103 || header[5]!=0 || header[6]!=0 || header[7]!=0 ||
        header[19]!=32 || header[20]!=4 || header[27]!=4096 || header[28]!=0 ||
        !w || !h || w%4 || h%4 || w>4096 || h>4096 ||
        (format!=D3DFMT_DXT1 && format!=D3DFMT_DXT2 && format!=D3DFMT_DXT3 && format!=D3DFMT_DXT4 && format!=D3DFMT_DXT5) ||
        info->Width!=w || info->Height!=h || info->Depth!=1 || info->MipLevels!=1 ||
        info->Format!=format || info->ResourceType!=D3DRTYPE_TEXTURE || info->ImageFileFormat!=D3DXIFF_DDS) return false;
    const UINT block=format==D3DFMT_DXT1?8:16;
    if(size!=128+(w/4)*(h/4)*block) return false;
    UINT targetW=1,targetH=1;
    while(targetW<w) targetW*=2;
    while(targetH<h) targetH*=2;
    auto original=reinterpret_cast<CreateFn>(0x4DE5CC);
    IDirect3DTexture9* candidate=nullptr;
    result=device->CreateTexture(targetW,targetH,1,0,format,D3DPOOL_MANAGED,&candidate,nullptr);
    D3DSURFACE_DESC desc{};
    bool valid=SUCCEEDED(result) && candidate && SUCCEEDED(candidate->GetLevelDesc(0,&desc)) &&
        desc.Width==targetW && desc.Height==targetH && desc.Format==format && candidate->GetLevelCount()==1;
    const UINT row=(w/4)*block,stride=(targetW/4)*block;
    if(valid) {
        D3DLOCKED_RECT locked{};
        valid=SUCCEEDED(candidate->LockRect(0,&locked,nullptr,0));
        if(valid) {
            valid=locked.Pitch>=int(stride);
            for(UINT y=0;valid && y<targetH/4;++y) {
                auto* dest=static_cast<uint8_t*>(locked.pBits)+y*locked.Pitch;
                if(y<h/4) {
                    std::memcpy(dest,static_cast<const uint8_t*>(data)+128+y*row,row);
                    std::memset(dest+row,0,stride-row);
                } else std::memset(dest,0,stride);
            }
            valid=SUCCEEDED(candidate->UnlockRect(0)) && valid;
        }
    }
    if(valid && verify) {
        D3DXIMAGE_INFO referenceInfo{};
        IDirect3DTexture9* reference=nullptr;
        auto ref=original(device,data,size,0,0,1,0,D3DFMT_UNKNOWN,D3DPOOL_MANAGED,
            D3DX_FILTER_NONE,D3DX_FILTER_NONE,0,&referenceInfo,nullptr,&reference);
        const bool sameInfo=std::memcmp(&referenceInfo,info,sizeof(referenceInfo))==0;
        if(SUCCEEDED(ref) && reference && Equal(candidate,reference)) ++legacyEqual; else ++legacyDifferent;
        // 再圧縮した旧結果ではなく、元素材の全ブロック＋ゼロ余白と実テクスチャを照合する。
        D3DLOCKED_RECT locked{};
        bool sourceEqual=SUCCEEDED(candidate->LockRect(0,&locked,nullptr,D3DLOCK_READONLY));
        if(sourceEqual) {
            sourceEqual=locked.Pitch>=int(stride);
            for(UINT y=0;sourceEqual && y<targetH/4;++y) {
                const auto* actual=static_cast<const uint8_t*>(locked.pBits)+y*locked.Pitch;
                const UINT used=y<h/4?row:0;
                if(used) sourceEqual=std::memcmp(actual,static_cast<const uint8_t*>(data)+128+y*row,used)==0;
                for(UINT x=used;sourceEqual && x<stride;++x) sourceEqual=actual[x]==0;
            }
            sourceEqual=SUCCEEDED(candidate->UnlockRect(0)) && sourceEqual;
        }
        valid=SUCCEEDED(ref) && reference && sameInfo && sourceEqual;
        if(reference) reference->Release();
        if(valid) ++directVerified; else ++directRejected;
    }
    if(!valid) {if(candidate) candidate->Release();return false;}
    // SourceInfoは呼出し直前のD3DXGetImageInfoが求めた元画像の情報を維持。
    *texture=candidate;++direct;return true;
}
__attribute__((force_align_arg_pointer)) HRESULT WINAPI Create(IDirect3DDevice9* device, const void* data, UINT size,
    UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
    DWORD filter, DWORD mipfilter, D3DCOLOR key, D3DXIMAGE_INFO* info,
    PALETTEENTRY* palette, IDirect3DTexture9** texture) {
    auto original=reinterpret_cast<CreateFn>(0x4DE5CC);
    const bool eligible=device && data && size>=4 && size<=Limit && !width && !height && levels==1 && !usage &&
        format==D3DFMT_UNKNOWN && pool==D3DPOOL_MANAGED && filter==D3DX_FILTER_NONE && mipfilter==D3DX_FILTER_NONE &&
        !key && !palette && info && texture && (firstFast || Adapter(device));
    if(!eligible) {++bypass;return original(device,data,size,width,height,levels,usage,format,pool,filter,mipfilter,key,info,palette,texture);}
    try {
        HRESULT directResult{};
        if(DirectDds(device,data,size,info,texture,directResult)) return directResult;
        // 初回経路は永続キャッシュを作らない。軽いBMP/PNGや対象外DDSは元APIで準備する。
        if(firstFast) {
            ++bypass;
            return original(device,data,size,width,height,levels,usage,format,pool,filter,mipfilter,key,info,palette,texture);
        }
        const auto path=Key(data,size);
        Header header{};
        std::vector<uint8_t> bytes;
        if(!path.empty() && Read(path,header,bytes)) {
            IDirect3DTexture9* cached=nullptr;
            D3DXIMAGE_INFO cachedInfo{};
            auto hr=original(device,bytes.data(),bytes.size(),width,height,levels,usage,format,pool,filter,mipfilter,key,&cachedInfo,nullptr,&cached);
            D3DSURFACE_DESC desc{};
            bool valid=SUCCEEDED(hr) && cached && SUCCEEDED(cached->GetLevelDesc(0,&desc)) &&
                desc.Width==header.width && desc.Height==header.height && unsigned(desc.Format)==header.format && cached->GetLevelCount()==1;
            if(valid && verify) {
                IDirect3DTexture9* reference=nullptr;
                D3DXIMAGE_INFO referenceInfo{};
                auto refHr=original(device,data,size,width,height,levels,usage,format,pool,filter,mipfilter,key,&referenceInfo,palette,&reference);
                valid=SUCCEEDED(refHr) && reference && std::memcmp(&referenceInfo,&header.info,sizeof(referenceInfo))==0 && Equal(cached,reference);
                if(reference) reference->Release();
                if(valid) ++verified; else ++rejected;
            }
            if(valid) {++hits;*texture=cached;*info=header.info;return hr;}
            if(cached) cached->Release();
        }
        ++misses;
        auto hr=original(device,data,size,width,height,levels,usage,format,pool,filter,mipfilter,key,info,palette,texture);
        if(SUCCEEDED(hr) && *texture && !path.empty()) {
            try { Write(path,*info,*texture); } catch(...) { /* 成功済みの元画像を維持 */ }
        }
        return hr;
    } catch(...) {
        // キャッシュのメモリ・ファイル操作が失敗しても元画像からの通常準備へ戻る。
        return original(device,data,size,width,height,levels,usage,format,pool,filter,mipfilter,key,info,palette,texture);
    }
}
bool Change(bool enable) {
    auto* site=reinterpret_cast<uint8_t*>(0x4BD3F2);
    auto expected=static_cast<int32_t>((enable?uintptr_t(0x4DE5CC):reinterpret_cast<uintptr_t>(&Create))-0x4BD3F7);
    if(site[0]!=0xE8 || std::memcmp(site+1,&expected,4)) return false;
    DWORD old{},ignored{};
    if(!VirtualProtect(site,5,PAGE_EXECUTE_READWRITE,&old)) return false;
    auto relative=static_cast<int32_t>((enable?reinterpret_cast<uintptr_t>(&Create):uintptr_t(0x4DE5CC))-0x4BD3F7);
    std::memcpy(site+1,&relative,4);
    const bool flush=FlushInstructionCache(GetCurrentProcess(),site,5)!=0;
    const bool protect=VirtualProtect(site,5,old,&ignored)!=0;
    if(!flush || !protect) ExitProcess(ERROR_WRITE_FAULT);
    active=enable;return true;
}
}
bool Active() {return active;}
void Initialize(uint8_t mode) {
    if (cccaster::diagnostics::startup::Baseline()) {
        cccaster::domain::session::DebugLog("[StartupAssets] disabled; original D3DX texture loading (no direct DDS or cache)");
        return;
    }
    if(mode>1 || !cccaster::diagnostics::startup::HasGate() ||
        std::getenv("CCCASTER_STARTUP_ASSETS_BASELINE") || !startup::MatchesMenuCode()) return;
    firstFast=std::getenv("CCCASTER_STARTUP_FIRST_BASELINE")==nullptr;
    verify=std::getenv("CCCASTER_STARTUP_ASSETS_VERIFY")!=nullptr;
    if(firstFast) {
        cccaster::domain::session::DebugLog("[StartupAssets] enabled=%u verify=%u direct=1",Change(true)?1:0,verify?1:0);
        return;
    }
    save=reinterpret_cast<SaveFn>(GetProcAddress(GetModuleHandleW(L"d3dx9_36.dll"),"D3DXSaveTextureToFileInMemory"));
    if(!save || BCryptOpenAlgorithmProvider(&sha,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) return;
    directory=cccaster::core::paths::Resolve("startup-textures-v2");
    if(const char* isolated=std::getenv("CCCASTER_STARTUP_CACHE_DIR")) directory=isolated;
    if(!CreateDirectoryA(directory.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS) return;
    cccaster::domain::session::DebugLog("[StartupAssets] enabled=%u verify=%u",Change(true)?1:0,verify?1:0);
}
void Restore(bool selectionReached) {
    if(!active) return;
    if(firstFast && !selectionReached) return;
    if(!Change(false)) ExitProcess(ERROR_WRITE_FAULT);
    cccaster::domain::session::DebugLog("[StartupAssets] restored=1 hits=%u misses=%u saved=%u bypass=%u verified=%u rejected=%u written=%llu",
        hits,misses,saved,bypass,verified,rejected,static_cast<unsigned long long>(written));
    cccaster::domain::session::DebugLog("[StartupDirectDDS] count=%u verified=%u rejected=%u legacyEqual=%u legacyDifferent=%u",direct,directVerified,directRejected,legacyEqual,legacyDifferent);
    if(sha) {BCryptCloseAlgorithmProvider(sha,0);sha=nullptr;}
}
}
