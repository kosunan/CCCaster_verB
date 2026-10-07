#pragma once
// 実ゲーム検証専用。描画済み画像を提示する直前の入力を、現在/直前の更新へ差し替える。
// 派生入力欄を書かず、GameMemのraw入口から標準メインループを実行し直す。
#include "core_dll/mbaa_mem/IGameMemory.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"
#include "core_dll/hook/DirectInputHook.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/common/Platform.hpp"
#include <windows.h>
#include <d3d9.h>
#include <array>
#include <cfenv>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace cccaster::testing::late_input {
using domain::session::DebugLog;
using game_interface::GameMem;
using game_interface::GameInput;
// 0=差替えなし、1=現在更新から、2=直前更新から。通常製品では未設定で無効。
inline int Depth() {
    static const int value = [] {
        const auto* v = std::getenv("CCCASTER_TEST_LATE_INPUT_ROLLBACK");
        return v && v[0] >= '0' && v[0] <= '2' && !v[1] ? v[0]-'0' : -1;
    }();
    return value;
}
inline bool NeutralControl() {
    static const bool enabled = [] {
        const auto* v=std::getenv("CCCASTER_TEST_LATE_INPUT_NEUTRAL");
        return v && v[0]=='1' && !v[1];
    }();
    return enabled;
}
struct Report {
    uint32_t magic, version, command, status, depth, trial, frame, worldBefore, worldAfter;
    uint32_t rawBefore, rawLatest, actorBefore, actorAfter, dirBefore, dirAfter;
    uint32_t queuedBefore, queuedAfter, motionBefore, motionAfter, pastPixelsDifferent;
    uint32_t pastImageEqual, replayCount, width, height, errors;
    uint32_t neutral, stateBytesDifferent, stateFirstDifference;
};
static_assert(sizeof(Report) == 112);
inline Report* report = nullptr;
inline HANDLE mapping = nullptr;
enum class Phase { Normal, Restore, Previous, Current };
inline Phase phase = Phase::Normal;
inline uint32_t frame = 0;
struct Snapshot {
    std::vector<char> bytes;
    std::fenv_t fp{};
    std::array<GameInput,2> inputs{};
    bool valid = false;
    void Save() {
        auto& mem = GameMem();
        bytes.resize(mem.PresentationSnapshotSize());
        const auto base = *reinterpret_cast<uintptr_t*>(CC_PTR_TO_WRITE_INPUT_ADDR);
        if (!base || bytes.empty() || !mem.SavePresentationSnapshot(bytes) || std::fegetenv(&fp)) {
            DebugLog("[LateInputRollback] FAIL save"); platform::TerminateSelf(); return;
        }
        for (unsigned i=0; i<2; ++i) {
            inputs[i].direction = *reinterpret_cast<uint32_t*>(base+0x18+i*0x14);
            inputs[i].buttons = *reinterpret_cast<uint16_t*>(base+0x24+i*0x14);
        }
        valid = true;
    }
    void Load() {
        if (!valid || !GameMem().LoadPresentationSnapshot(bytes) || std::fesetenv(&fp)) {
            DebugLog("[LateInputRollback] FAIL restore"); platform::TerminateSelf();
        }
    }
};
inline Snapshot previous, current;
inline std::array<GameInput,2> replacement{};
inline std::vector<uint32_t> lastImage, image;
inline std::vector<char> originalState, correctedState;
inline IDirect3DSurface9* readable = nullptr;
inline D3DSURFACE_DESC readableDesc{};
inline bool Replaying() { return phase == Phase::Previous || phase == Phase::Current; }
inline bool SkipPresent() { return phase == Phase::Restore || phase == Phase::Previous; }
inline void WriteImage(const char* name,const std::vector<uint32_t>& pixels) {
    const auto* directory=std::getenv("CCCASTER_TEST_LATE_INPUT_DIRECTORY");
    if (!directory || report->trial!=1 || pixels.size()!=size_t(report->width)*report->height) return;
    BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};
    file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+pixels.size()*4;
    info.biSize=sizeof(info);info.biWidth=report->width;info.biHeight=-int(report->height);info.biPlanes=1;info.biBitCount=32;
    std::ofstream out(std::filesystem::path(directory)/name,std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file),sizeof(file));out.write(reinterpret_cast<const char*>(&info),sizeof(info));
    out.write(reinterpret_cast<const char*>(pixels.data()),pixels.size()*4);
}
inline void Fail(const char* why) {
    if (report) { ++report->errors; report->status=5; MemoryBarrier(); }
    DebugLog("[LateInputRollback] FAIL %s", why); platform::TerminateSelf();
}
inline void Initialize() {
    if (report || Depth()<0) return;
    wchar_t name[96]; swprintf(name,96,L"Local\\CCCasterLateInput_%lu",GetCurrentProcessId());
    mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,4096,name);
    report=static_cast<Report*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,4096));
    if (!report) { Fail("mapping"); return; }
    std::memset(report,0,sizeof(*report)); report->magic=0x4c495242;report->version=2;report->depth=Depth();
    report->neutral=NeutralControl();
}
inline void CaptureImage(IDirect3DDevice9* device) {
    IDirect3DSurface9* surface=nullptr;
    if (FAILED(device->GetRenderTarget(0,&surface))) { Fail("render_target"); return; }
    D3DSURFACE_DESC desc{}; surface->GetDesc(&desc);
    if (!readable || desc.Width!=readableDesc.Width || desc.Height!=readableDesc.Height || desc.Format!=readableDesc.Format) {
        if (readable) { readable->Release(); readable=nullptr; }
        if (FAILED(device->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&readable,nullptr))) {
            surface->Release(); Fail("readable_surface"); return;
        }
        readableDesc=desc;
    }
    const auto copied=device->GetRenderTargetData(surface,readable); surface->Release();
    if (FAILED(copied) || (desc.Format!=D3DFMT_X8R8G8B8 && desc.Format!=D3DFMT_A8R8G8B8)) { Fail("readback"); return; }
    D3DLOCKED_RECT locked{};
    if (FAILED(readable->LockRect(&locked,nullptr,D3DLOCK_READONLY))) { Fail("lock"); return; }
    image.resize(size_t(desc.Width)*desc.Height);
    for (unsigned y=0;y<desc.Height;++y)
        std::memcpy(image.data()+size_t(y)*desc.Width,static_cast<char*>(locked.pBits)+size_t(y)*locked.Pitch,desc.Width*4);
    readable->UnlockRect(); report->width=desc.Width; report->height=desc.Height;
}
inline void ReadActor(bool after) {
    const uint32_t combined=*reinterpret_cast<uint32_t*>(0x55541c);
    const uint32_t direction=*reinterpret_cast<uint8_t*>(0x55541b);
    const uint32_t queued=*reinterpret_cast<uint16_t*>(0x55543c);
    const uint32_t motion=*reinterpret_cast<uint32_t*>(0x555140);
    if (after) { report->actorAfter=combined;report->dirAfter=direction;report->queuedAfter=queued;report->motionAfter=motion; }
    else { report->actorBefore=combined;report->dirBefore=direction;report->queuedBefore=queued;report->motionBefore=motion; }
}
// trueなら古い画像のHUD/Presentを省略し、AfterPresentから再計算を始める。
inline bool BeforePresent(IDirect3DDevice9* device, bool eligible) {
    if (Depth()<0) return false;
    Initialize();
    if (!eligible && !Replaying()) { previous.valid=current.valid=false; lastImage.clear(); return false; }
    CaptureImage(device);
    if (phase==Phase::Previous) {
        uint32_t different=0;
        if (image.size()!=lastImage.size()) different=UINT32_MAX;
        else for (size_t i=0;i<image.size();++i) if ((image[i]&0xffffff)!=(lastImage[i]&0xffffff)) ++different;
        report->pastPixelsDifferent=different; report->pastImageEqual=different==0;
        WriteImage("late_original_previous.bmp",lastImage);WriteImage("late_corrected_previous.bmp",image);
        DebugLog("[LateInputRollback] previous trial=%u world=%u differentPixels=%u",report->trial,GameMem().WorldTimer(),different);
        return true;
    }
    if (phase==Phase::Current) {
        report->worldAfter=GameMem().WorldTimer(); ReadActor(true); lastImage=image;
        if (NeutralControl()) {
            correctedState.resize(originalState.size());
            if (!GameMem().SaveTrainingSnapshot(correctedState)) { Fail("control_state"); return true; }
            report->stateBytesDifferent=0;report->stateFirstDifference=UINT32_MAX;
            for (size_t i=0;i<originalState.size();++i) if (originalState[i]!=correctedState[i]) {
                if (!report->stateBytesDifferent) report->stateFirstDifference=i;
                ++report->stateBytesDifferent;
            }
            DebugLog("[LateInputRollback] neutral trial=%u stateDifferent=%u first=%u bytes=%u",report->trial,
                report->stateBytesDifferent,report->stateFirstDifference,unsigned(originalState.size()));
        }
        WriteImage("late_corrected_current.bmp",image);
        if (report->worldAfter!=report->worldBefore) { Fail("simulation_time_advanced"); return true; }
        DebugLog("[LateInputRollback] corrected trial=%u depth=%u world=%u motion=%u actor=%u dir=%u queued=%u",
            report->trial,report->depth,report->worldAfter,report->motionAfter,report->actorAfter,report->dirAfter,report->queuedAfter);
        return false;
    }
    MemoryBarrier();
    if (report->command==1 && previous.valid && current.valid && !lastImage.empty()) {
        ++report->trial;report->frame=frame;report->worldBefore=GameMem().WorldTimer();
        report->rawBefore=current.inputs[0].Pack(); ReadActor(false);
        if (NeutralControl()) {
            originalState.resize(GameMem().TrainingSnapshotSize());
            if (originalState.empty() || !GameMem().SaveTrainingSnapshot(originalState)) { Fail("control_state"); return true; }
        }
        report->pastPixelsDifferent=0;report->pastImageEqual=0;report->replayCount=0;
        report->status=2;MemoryBarrier();
        // 外部の仮想パッドから、既存入力を処理した後に値を変えるための検証ゲート。
        // 性能測定には使わず、全depthに同じ条件で設ける。
        const auto until=GetTickCount64()+3000;
        while (report->command!=2) {
            if (GetTickCount64()>until) { Fail("external_pad_timeout"); return true; }
            Sleep(0);MemoryBarrier();
        }
        // 仮想パッド提出完了とDirectInputへの反映は別。実APIで変化を受け取るまで観測する。
        const auto receivedUntil=GetTickCount64()+200;
        do {
            game_interface::DirectInputHook::Poll();
            replacement={GameInput::Unpack(game_interface::DirectInputHook::GetPlayer1Input()),current.inputs[1]};
            if (replacement[0].Pack()!=report->rawBefore) break;
            Sleep(1);
        } while (GetTickCount64()<receivedUntil);
        report->rawLatest=replacement[0].Pack();report->command=0;
        DebugLog("[LateInputRollback] sample trial=%u depth=%u world=%u rawBefore=%08X latest=%08X motion=%u actor=%u dir=%u queued=%u",
            report->trial,report->depth,report->worldBefore,report->rawBefore,report->rawLatest,report->motionBefore,
            report->actorBefore,report->dirBefore,report->queuedBefore);
        if (report->rawBefore==report->rawLatest) { Fail("input_did_not_change"); return true; }
        // 同じ入力でも画像差が出るかを確認する対照。新しい実入力は記録し、再計算には使わない。
        if (NeutralControl()) replacement=current.inputs;
        WriteImage("late_original_current.bmp",image);
        if (Depth()) { phase=Phase::Restore;report->status=3;return true; }
        report->worldAfter=GameMem().WorldTimer();ReadActor(true);report->status=4;MemoryBarrier();
    }
    lastImage=image;
    return false;
}
inline bool AfterPresent() {
    if (Depth()<0) return false;
    if (phase==Phase::Restore) {
        (Depth()==2 ? previous : current).Load();
        GameMem().WriteInput(replacement[0],replacement[1]);
        GameMem().SetPresentationPreview(true);
        phase=Depth()==2 ? Phase::Previous : Phase::Current;
        return true;
    }
    if (phase==Phase::Previous) {
        ++report->replayCount;
        // 次の試行が古い入力履歴へ戻らないよう、訂正後の現在更新の開始状態を保持。
        GameMem().WriteInput(replacement[0],replacement[1]);current.Save();
        phase=Phase::Current;return true;
    }
    if (phase==Phase::Current) {
        ++report->replayCount;GameMem().SetPresentationPreview(false);phase=Phase::Normal;
        report->status=4;MemoryBarrier();
    }
    return false;
}
inline void AfterStep(bool eligible) {
    if (Depth()<0) return;
    Initialize();
    if (!eligible) { previous.valid=current.valid=false;lastImage.clear();return; }
    std::swap(previous,current);current.Save();++frame;
    if (report->status==0) report->status=1;
}
}
