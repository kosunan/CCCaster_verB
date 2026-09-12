#pragma once
// 明示指定時だけD3D9呼出しを計測。GPU完了時間ではなくAPI内の実QPC経過時間。
#include <d3d9.h>
#include <MinHook.h>
#include <array>
#include <algorithm>
#include <tuple>
#include <cstdlib>
#include "core_dll/common/Platform.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/sync/NetplaySession.hpp"
#include "core_dll/timing/SpeedFlags.hpp"

namespace cccaster::game_interface::render_probe {
inline bool Enabled() {
    static const bool enabled = std::getenv("CCCASTER_RENDER_PROBE") != nullptr;
    return enabled;
}
struct Metric { unsigned count = 0, failed = 0; int64_t ticks = 0, maximum = 0; };
inline std::array<void *, 119> originals{};
inline std::array<const char *, 119> names{};
inline thread_local std::array<Metric, 119> metrics{};
inline thread_local bool ignored = false;
inline thread_local unsigned passes = 0, serial = 0, passDraws = 0;
inline thread_local int64_t passApiTicks = 0;
inline thread_local uintptr_t beginCaller = 0;
inline thread_local unsigned passCalls = 0;
inline void Record(unsigned index, int64_t started, HRESULT result) {
    const auto ticks = platform::RealMonotonicTicks() - started;
    auto &m = metrics[index];
    ++m.count;
    m.failed += FAILED(result);
    m.ticks += ticks;
    m.maximum = std::max(m.maximum, ticks);
    passApiTicks += ticks;
    ++passCalls;
    if (index >= 81 && index <= 84) ++passDraws;
}
template<unsigned Index, class... Args>
HRESULT WINAPI Call(IDirect3DDevice9 *device, Args... args) {
    using Fn = HRESULT(WINAPI *)(IDirect3DDevice9 *, Args...);
    const auto original = reinterpret_cast<Fn>(originals[Index]);
    if (ignored) return original(device, args...);
    if constexpr (Index == 41)
        beginCaller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if constexpr (Index == 65) {
        if (std::getenv("CCCASTER_TEXTURE_TRANSFER_PROBE")) {
            auto texture=std::get<1>(std::make_tuple(args...));
            if(texture) {
                const auto t=platform::RealMonotonicTicks();
                texture->PreLoad();
                const auto end=platform::RealMonotonicTicks();
                const auto elapsed=end-t;
                if(elapsed>6000) domain::session::DebugLog("[TextureTransfer] f=%u texture=%p ticks=%lld begin=%lld end=%lld pid=%u tid=%u",core::netplay::NetplaySession::GetState().appliedFrame.load(),texture,elapsed,t,end,platform::ProcessId(),platform::ThreadId());
            }
        }
    }
    const auto started = platform::RealMonotonicTicks();
    const auto result = original(device, args...);
    Record(Index, started, result);
    if constexpr (Index == 84) {
        const auto ended = platform::RealMonotonicTicks();
        const auto elapsed = ended - started;
        if (elapsed > 6000) {
            domain::session::DebugLog("[DrawWork] f=%u begin=%lld end=%lld pid=%u tid=%u",core::netplay::NetplaySession::GetState().appliedFrame.load(),started,ended,platform::ProcessId(),platform::ThreadId());
            if(std::getenv("CCCASTER_DRAW_STATE")) {
                const auto f=core::netplay::NetplaySession::GetState().appliedFrame.load();
                const auto tuple=std::make_tuple(args...);
                domain::session::DebugLog("[DrawGeometry] f=%u caller=%p type=%u vertices=%u primitives=%u stride=%u",f,__builtin_return_address(0),
                    unsigned(std::get<0>(tuple)),unsigned(std::get<2>(tuple)),unsigned(std::get<3>(tuple)),unsigned(std::get<7>(tuple)));
                for(unsigned i=0;i<210;++i) {DWORD value=0;
                    if(SUCCEEDED(device->GetRenderState(D3DRENDERSTATETYPE(i),&value)))
                        domain::session::DebugLog("[DrawRS] f=%u i=%u v=%u",f,i,value);
                }
                for(unsigned stage=0;stage<2;++stage) for(unsigned i=0;i<33;++i) {DWORD value=0;
                    if(SUCCEEDED(device->GetTextureStageState(stage,D3DTEXTURESTAGESTATETYPE(i),&value)))
                        domain::session::DebugLog("[DrawTSS] f=%u s=%u i=%u v=%u",f,stage,i,value);
                }
                for(unsigned i=1;i<14;++i) {DWORD value=0;
                    if(SUCCEEDED(device->GetSamplerState(0,D3DSAMPLERSTATETYPE(i),&value)))
                        domain::session::DebugLog("[DrawSS] f=%u i=%u v=%u",f,i,value);
                }
            }
            IDirect3DPixelShader9 *ps = nullptr;
            IDirect3DVertexShader9 *vs = nullptr;
            DWORD fvf=0,src=0,dst=0,op=0,alpha=0,test=0,z=0;
            DWORD co=0,c1=0,c2=0,ao=0,a1=0,a2=0,cnext=0,light=0;
            device->GetPixelShader(&ps); device->GetVertexShader(&vs); device->GetFVF(&fvf);
            device->GetRenderState(D3DRS_SRCBLEND,&src); device->GetRenderState(D3DRS_DESTBLEND,&dst);
            device->GetRenderState(D3DRS_BLENDOP,&op); device->GetRenderState(D3DRS_ALPHABLENDENABLE,&alpha);
            device->GetRenderState(D3DRS_ALPHATESTENABLE,&test); device->GetRenderState(D3DRS_ZENABLE,&z);
            device->GetRenderState(D3DRS_LIGHTING,&light);
            device->GetTextureStageState(0,D3DTSS_COLOROP,&co); device->GetTextureStageState(0,D3DTSS_COLORARG1,&c1);
            device->GetTextureStageState(0,D3DTSS_COLORARG2,&c2); device->GetTextureStageState(0,D3DTSS_ALPHAOP,&ao);
            device->GetTextureStageState(0,D3DTSS_ALPHAARG1,&a1); device->GetTextureStageState(0,D3DTSS_ALPHAARG2,&a2);
            device->GetTextureStageState(1,D3DTSS_COLOROP,&cnext);
            IDirect3DBaseTexture9 *texture=nullptr;
            D3DSURFACE_DESC textureDesc{};
            device->GetTexture(0,&texture);
            if(texture && texture->GetType()==D3DRTYPE_TEXTURE)
                static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0,&textureDesc);
            domain::session::DebugLog("[DrawSlow] f=%u ticks=%lld ps=%p vs=%p fvf=%u src=%u dst=%u op=%u alpha=%u test=%u z=%u caller=%p",
                core::netplay::NetplaySession::GetState().appliedFrame.load(),elapsed,ps,vs,fvf,src,dst,op,alpha,test,z,__builtin_return_address(0));
            domain::session::DebugLog("[DrawSlowState] f=%u co=%u c1=%u c2=%u ao=%u a1=%u a2=%u cnext=%u light=%u",
                core::netplay::NetplaySession::GetState().appliedFrame.load(),co,c1,c2,ao,a1,a2,cnext,light);
            domain::session::DebugLog("[DrawSlowTexture] f=%u texture=%p w=%u h=%u format=%u pool=%u",
                core::netplay::NetplaySession::GetState().appliedFrame.load(),texture,textureDesc.Width,textureDesc.Height,unsigned(textureDesc.Format),unsigned(textureDesc.Pool));
            IDirect3DSurface9 *rt=nullptr,*depthSurface=nullptr;
            D3DSURFACE_DESC rtDesc{},depthDesc{};
            if(SUCCEEDED(device->GetRenderTarget(0,&rt))) {rt->GetDesc(&rtDesc);rt->Release();}
            if(SUCCEEDED(device->GetDepthStencilSurface(&depthSurface))) {depthSurface->GetDesc(&depthDesc);depthSurface->Release();}
            domain::session::DebugLog("[DrawSlowTarget] f=%u w=%u h=%u format=%u msaa=%u depth=%u",core::netplay::NetplaySession::GetState().appliedFrame.load(),
                rtDesc.Width,rtDesc.Height,unsigned(rtDesc.Format),unsigned(rtDesc.MultiSampleType),unsigned(depthDesc.Format));
            if(texture) texture->Release();
            if(ps) ps->Release(); if(vs) vs->Release();
        }
    }
    return result;
}
template<unsigned Index, class... Args>
void Hook(void **table, const char *name) {
    const auto created = MH_CreateHook(table[Index], reinterpret_cast<void *>(&Call<Index, Args...>),
                                       &originals[Index]);
    const auto enabled = created == MH_OK ? MH_EnableHook(table[Index]) : created;
    names[Index] = name;
    domain::session::DebugLog("[RenderProbeHook] api=%s create=%d enable=%d", name, int(created), int(enabled));
}
inline void Install(IDirect3DDevice9 *device) {
    if (!Enabled()) return;
    static bool installed = false;
    if (installed) return;
    installed = true;
    auto table = *reinterpret_cast<void ***>(device);
    Hook<30, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const POINT *>(table, "UpdateSurface");
    Hook<31, IDirect3DBaseTexture9 *, IDirect3DBaseTexture9 *>(table, "UpdateTexture");
    Hook<32, IDirect3DSurface9 *, IDirect3DSurface9 *>(table, "GetRenderTargetData");
    Hook<34, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE>(table, "StretchRect");
    Hook<35, IDirect3DSurface9 *, const RECT *, D3DCOLOR>(table, "ColorFill");
    Hook<37, DWORD, IDirect3DSurface9 *>(table, "SetRenderTarget");
    names[41] = "BeginScene"; // DxHookが通常経路と計測を共用する。
    names[42] = "EndScene";
    Hook<43, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD>(table, "Clear");
    Hook<57, D3DRENDERSTATETYPE, DWORD>(table, "SetRenderState");
    Hook<65, DWORD, IDirect3DBaseTexture9 *>(table, "SetTexture");
    Hook<67, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD>(table, "SetTextureStageState");
    Hook<69, DWORD, D3DSAMPLERSTATETYPE, DWORD>(table, "SetSamplerState");
    Hook<81, D3DPRIMITIVETYPE, UINT, UINT>(table, "DrawPrimitive");
    Hook<82, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT>(table, "DrawIndexedPrimitive");
    Hook<83, D3DPRIMITIVETYPE, UINT, const void *, UINT>(table, "DrawPrimitiveUP");
    Hook<84, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT>(table, "DrawIndexedPrimitiveUP");
    Hook<94, UINT, const float *, UINT>(table, "SetVertexShaderConstantF");
    Hook<109, UINT, const float *, UINT>(table, "SetPixelShaderConstantF");
}
inline void EndPass(IDirect3DDevice9 *device, uintptr_t endCaller) {
    ++passes;
    const auto frame = core::netplay::NetplaySession::GetState().appliedFrame.load();
    if (frame && frame % 120 == 0) {
        IDirect3DSurface9 *target = nullptr, *back = nullptr;
        D3DSURFACE_DESC desc{};
        device->GetRenderTarget(0, &target);
        device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
        if (target) target->GetDesc(&desc);
        domain::session::DebugLog("[RenderPass] seq=%u f=%u pass=%u target=%p back=%d w=%u h=%u draws=%u apiTicks=%lld calls=%u begin=%08X end=%08X",
            serial, frame, passes, target, target && target == back, desc.Width, desc.Height, passDraws, passApiTicks,
            passCalls, unsigned(beginCaller), unsigned(endCaller));
        if (target) target->Release();
        if (back) back->Release();
    }
    passDraws = 0;
    passApiTicks = 0;
    passCalls = 0;
}
inline void FinishFrame() {
    if (!Enabled()) return;
    const auto frame = core::netplay::NetplaySession::GetState().appliedFrame.load();
    const bool skip = core::SpeedFlags::RenderSkip().load();
    domain::session::DebugLog("[RenderFrame] seq=%u f=%u skip=%d passes=%u", serial, frame, skip, passes);
    for (unsigned i = 0; i < metrics.size(); ++i) {
        const auto &m = metrics[i];
        if (m.count)
            domain::session::DebugLog("[RenderApi] seq=%u f=%u skip=%d api=%s count=%u ticks=%lld maxTicks=%lld failed=%u",
                serial, frame, skip, names[i], m.count, m.ticks, m.maximum, m.failed);
    }
    metrics = {};
    passes = passDraws = 0;
    passApiTicks = 0;
    passCalls = 0;
    ++serial;
}
} // namespace cccaster::game_interface::render_probe
