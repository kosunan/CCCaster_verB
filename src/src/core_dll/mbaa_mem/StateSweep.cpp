#include "core_dll/mbaa_mem/StateSweep.hpp"
#include "core_dll/mbaa_mem/IGameMemory.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"
#include "core_dll/mbaa_mem/GameBuildGuard.hpp"
#include "core_dll/engine/SceneFastBoot.hpp"
#include "core_dll/timing/SpeedFlags.hpp"
#include "core_dll/timing/FrameTiming.hpp"
#include "core_dll/hook/TimeHooks.hpp"
#include "core_dll/common/Platform.hpp"
#include "core_dll/sync/SelectionState.hpp"
#include <windows.h>
#include <d3d9.h>
#include <MinHook.h>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>

// 0x45F2D0: EAX=ActorData。0x45F450: cdecl(ActorData*)。
// 0x426940/0x426946にも同じ順の初期化がある。ABIは命令列で確認。
extern "C" __attribute__((naked)) void cc_sweep_reset_actor(void*) {
    __asm__ __volatile__("movl 4(%esp),%eax; movl $0x45F2D0,%edx; jmp *%edx");
}

namespace cccaster::testing::state_sweep {
namespace {
using game_interface::GameMem;
struct Case { uint32_t slot, pattern, state, duration, animation; };
struct Range { uintptr_t address; size_t length; };
// 保存表から生成しない独立した監視候補。差を即保存漏れと断定せず、生の候補として報告。
constexpr Range watch[] = {{0x55512C,4}, {0x5581D0,1048}, {0x558600,8},
                           {0x55D1CC,4}, {0x56406C,4}, {0x76E008,1500}};
FILE* report = nullptr;
std::filesystem::path output;
std::vector<Case> cases;
std::vector<char> baseline, saved, first, second, restored, observed0, observed1, observed2;
size_t current = 0;
uint32_t character = 0, moon = 0, nav = 0, stage = 0, warmup = 0;
int phase = 0;
int patternFilter = -1, stateFilter = -1;
bool initialized = false, catalogOnly = false;
bool entryOnly = false;
uint32_t maxStateFrames = 600, caseFrame = 0, stateAge = 0, simulationFrame = 0;
std::vector<bool> coveredAges;
uint64_t endedCases = 0, nominalFrames = 0, coveredFrames = 0, caseCovered = 0;
uint64_t checked = 0, mismatches = 0, watchChanges = 0;
uint64_t drawCallsSkipped = 0;
int64_t started = 0, scanStarted = 0;
uint32_t setupMode = UINT32_MAX, setupIntro = UINT32_MAX, skipFrames = 0;
uint64_t loadingSkipPulses = 0, introSkipPulses = 0;
int64_t loadingStarted = 0, loadingUs = 0, combatReadyUs = 0;
size_t definitionCount = 0;
std::vector<std::pair<void*, void*>> drawingHooks;

// Present省略だけではゲーム本体のラスタライズが残る。検証プロセス内のD3D API側で省略。
// リソース作成・更新・解放とゲームの描画準備は残し、寿命や状態遷移を変えない。
template<unsigned Index, class... Args>
HRESULT WINAPI SkipDraw(IDirect3DDevice9* device, Args...) {
    ++drawCallsSkipped;
    // UP系APIが持つ呼出し後のストリーム／index解除は再現する。
    if constexpr (Index == 83 || Index == 84) device->SetStreamSource(0, nullptr, 0, 0);
    if constexpr (Index == 84) device->SetIndices(nullptr);
    return D3D_OK;
}
template<unsigned Index, class... Args> void HookDraw(void** table) {
    void* original = nullptr;
    if (MH_CreateHook(table[Index], reinterpret_cast<void*>(&SkipDraw<Index, Args...>), &original) != MH_OK ||
        MH_EnableHook(table[Index]) != MH_OK) throw std::runtime_error("draw_hook_failed");
    drawingHooks.emplace_back(table[Index], original);
}
void DisableDrawing(IDirect3DDevice9* device) {
    if (!device) throw std::runtime_error("device_required");
    const auto table = *reinterpret_cast<void***>(device);
    HookDraw<81, D3DPRIMITIVETYPE, UINT, UINT>(table);
    HookDraw<82, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT>(table);
    HookDraw<83, D3DPRIMITIVETYPE, UINT, const void*, UINT>(table);
    HookDraw<84, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT>(table);
    HookDraw<43, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD>(table);
    HookDraw<34, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE>(table);
    HookDraw<35, IDirect3DSurface9*, const RECT*, D3DCOLOR>(table);
}

template<class T> T Read(uintptr_t address) {
    T value{};
    SIZE_T read = 0;
    if (!address || !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
                                       &value, sizeof(value), &read) || read != sizeof(value))
        throw std::runtime_error("unreadable_game_data");
    return value;
}
template<class T> void Write(uintptr_t address, T value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
}
uint32_t Item(uint32_t table, uint32_t index, uint32_t maximum) {
    const auto count = Read<uint32_t>(table + 12);
    const auto size = Read<uint32_t>(table + 8);
    if (count > maximum || index >= count) throw std::runtime_error("container_bounds");
    const auto base = Read<uint32_t>(table + 4);
    if (!base) throw std::runtime_error("container_null");
    if (Read<uint32_t>(table) != 0) return Read<uint32_t>(base + 4 * index);
    if (!size || size > 4096 || uint64_t(base) + uint64_t(size) * index > UINT32_MAX)
        throw std::runtime_error("container_stride");
    return base + size * index;
}
int Option(const char* name, int fallback, int min, int max) {
    const auto* value = std::getenv(name);
    if (!value) return fallback;
    int result = 0;
    const auto end = value + std::strlen(value);
    const auto parsed = std::from_chars(value, end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end || result < min || result > max)
        throw std::runtime_error("invalid_option");
    return result;
}
void Flush() { if (report) std::fflush(report); }
[[noreturn]] void Finish(const char* status, const char* reason = "") {
    if (report) {
        std::fprintf(report, "{\"event\":\"end\",\"status\":\"%s\",\"reason\":\"%s\","
            "\"defined\":%zu,\"checked\":%llu,\"mismatches\":%llu,\"watch_changes\":%llu,\"elapsed_us\":%lld,"
            "\"scan_us\":%lld,\"draw_calls_skipped\":%llu,\"loading_us\":%lld,"
            "\"combat_ready_us\":%lld,\"loading_skip_pulses\":%llu,\"intro_skip_pulses\":%llu,"
            "\"ended_cases\":%llu,\"nominal_frame_points\":%llu,\"covered_frame_points\":%llu}\n",
            status, reason, cases.size(), checked, mismatches, watchChanges,
            platform::RealMonotonicUs() - started,
            scanStarted ? platform::RealMonotonicUs() - scanStarted : 0, drawCallsSkipped,
            loadingUs, combatReadyUs, loadingSkipPulses, introSkipPulses,
            endedCases, nominalFrames, coveredFrames);
        std::fclose(report); report = nullptr;
    }
    // 検証専用コピーを外側のrunnerが回収する。設定の保存処理へ進まない。
    ExitProcess(std::strcmp(status, "complete") == 0 || std::strcmp(status, "catalog") == 0 ? 0 : 2);
}
void Dump(const char* name, const std::vector<char>& bytes) {
    const auto path = output / name;
    auto* file = _wfopen(path.c_str(), L"wb");
    if (!file) throw std::runtime_error("dump_open_failed");
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose(file);
    if (!ok) throw std::runtime_error("dump_write_failed");
}
void Observe(std::vector<char>& bytes) {
    bytes.clear();
    for (const auto& r : watch) {
        const auto pos = bytes.size();
        bytes.resize(pos + r.length);
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(r.address),
                bytes.data() + pos, r.length, &read) || read != r.length)
            throw std::runtime_error("watch_read_failed");
    }
}
void WatchDiff(const std::vector<char>& a, const std::vector<char>& b, const char* kind) {
    size_t base = 0;
    for (const auto& range : watch) {
        size_t count = 0, firstOffset = 0;
        for (size_t i = 0; i < range.length; ++i) if (a[base+i] != b[base+i]) {
            if (!count) firstOffset = i;
            ++count;
        }
        if (count) {
            ++watchChanges;
            std::fprintf(report, "{\"event\":\"watch\",\"case\":%zu,\"kind\":\"%s\","
                "\"address\":%u,\"first_offset\":%zu,\"different_bytes\":%zu}\n",
                current, kind, unsigned(range.address), firstOffset, count);
        }
        base += range.length;
    }
}
void Save(std::vector<char>& bytes) {
    if (!GameMem().SaveSnapshot(bytes)) throw std::runtime_error("snapshot_save_failed");
}
void Load(std::vector<char>& bytes) {
    if (!GameMem().LoadSnapshot(bytes)) throw std::runtime_error("snapshot_restore_failed");
}
void Enumerate() {
    for (unsigned slot = 0; slot < 4; ++slot) {
        const auto player = 0x555130 + slot * 0xAFC;
        if (!Read<uint8_t>(player)) continue;
        const auto table = Read<uint32_t>(Read<uint32_t>(Read<uint32_t>(player + 0x330)) + 4);
        const auto count = Read<uint32_t>(table + 12);
        if (!count || count > 10000) throw std::runtime_error("pattern_count");
        for (unsigned pattern = 0; pattern < count; ++pattern) {
            const auto data = Item(table, pattern, 10000);
            if (!data) continue;
            const auto animationTable = Read<uint32_t>(data + 0x34);
            const auto states = Read<uint32_t>(animationTable + 12);
            if (states > 10000) throw std::runtime_error("state_count");
            for (unsigned state = 0; state < states; ++state) {
                const auto animation = Item(animationTable, state, 10000);
                if (!animation) throw std::runtime_error("animation_null");
                // 0x461150は+0xCのunsigned shortとActor+0x18を比較する。
                const auto duration = Read<uint16_t>(animation + 0xC);
                ++definitionCount;
                const bool selected = (patternFilter < 0 || pattern == unsigned(patternFilter)) &&
                                      (stateFilter < 0 || state == unsigned(stateFilter));
                std::fprintf(report, "{\"event\":\"definition\",\"slot\":%u,\"actor_character\":%u,"
                    "\"actor_moon\":%u,\"pattern\":%u,\"state\":%u,\"duration\":%u,\"animation_type\":%u,"
                    "\"selected\":%s}\n", slot, unsigned(Read<uint8_t>(player+5)),
                    unsigned(Read<uint16_t>(player+12)), pattern, state, unsigned(duration),
                    unsigned(Read<uint8_t>(animation+14)), selected ? "true" : "false");
                if (selected) {
                    cases.push_back({slot, pattern, state, duration, animation});
                    nominalFrames += std::max(1U, unsigned(duration));
                }
            }
        }
    }
    std::fprintf(report, "{\"event\":\"catalog_end\",\"definitions\":%zu,\"selected\":%zu,"
        "\"nominal_frame_points\":%llu}\n", definitionCount, cases.size(), nominalFrames);
    Flush();
    if (cases.empty()) throw std::runtime_error("no_cases");
}
void BeginFrame() {
    GameMem().WriteInput({}, {});
    stateAge = Read<uint32_t>(0x555134 + cases[current].slot * 0xAFC + 0x18);
    std::fprintf(report, "{\"event\":\"frame_begin\",\"case\":%zu,\"frame\":%u,\"state_age\":%u}\n",
                 current, caseFrame, stateAge);
    Flush();
    Save(saved); Observe(observed0);
    simulationFrame = 65537 + uint32_t(checked);
    GameMem().BeginSimulation(simulationFrame);
    phase = 1;
}
void Next() {
    if (current == cases.size()) Finish("complete");
    const auto& c = cases[current];
    // 先にチェックポイントを永続化。ネイティブ関数の例外・ハングも未検査と区別する。
    std::fprintf(report, "{\"event\":\"begin_case\",\"case\":%zu,\"slot\":%u,\"pattern\":%u,\"state\":%u}\n",
                 current, c.slot, c.pattern, c.state);
    Flush();
    Load(baseline);
    caseFrame = 0; caseCovered = 0;
    coveredAges.assign(std::max(1U, c.duration), false);
    const auto actor = 0x555134 + c.slot * 0xAFC;
    Write<uint32_t>(actor + 0xC, c.pattern);
    Write<uint32_t>(actor + 0x10, c.state);
    cc_sweep_reset_actor(reinterpret_cast<void*>(actor));
    reinterpret_cast<int (__cdecl*)(void*)>(0x45F450)(reinterpret_cast<void*>(actor));
    if (Read<uint32_t>(actor+0xC) != c.pattern || Read<uint32_t>(actor+0x10) != c.state ||
        Read<uint32_t>(actor+0x31C) != c.animation)
        throw std::runtime_error("entry_rejected_or_redirected");
    BeginFrame();
}
void Initialize(const wchar_t* directory, IDirect3DDevice9* device) {
    started = platform::RealMonotonicUs();
    output = std::filesystem::path(directory);
    if (!output.is_absolute() || !std::filesystem::is_directory(output))
        throw std::runtime_error("output_directory_required");
    if (std::filesystem::exists(output / "events.jsonl"))
        throw std::runtime_error("report_already_exists");
    report = _wfopen((output / "events.jsonl").c_str(), L"wb");
    if (!report) throw std::runtime_error("report_exists_or_unwritable");
    character = Option("CCCASTER_SWEEP_CHARACTER", 0, 0, 100);
    moon = Option("CCCASTER_SWEEP_MOON", 0, 0, 2);
    patternFilter = Option("CCCASTER_SWEEP_PATTERN", -1, 0, 9999);
    stateFilter = Option("CCCASTER_SWEEP_STATE", -1, 0, 9999);
    catalogOnly = Option("CCCASTER_SWEEP_CATALOG", 0, 0, 1) != 0;
    entryOnly = Option("CCCASTER_SWEEP_ENTRY_ONLY", 0, 0, 1) != 0;
    maxStateFrames = Option("CCCASTER_SWEEP_MAX_STATE_FRAMES", 600, 1, 100000);
    constexpr unsigned char reset[] = {0x53,0x33,0xdb,0x56,0x8b,0xf0,0x57};
    constexpr unsigned char apply[] = {0x55,0x8b,0xec,0x83,0xe4,0xf8,0x83,0xec,0x0c};
    if (!game_build::RuntimeValidated() ||
        std::memcmp(reinterpret_cast<void*>(0x45F2D0), reset, sizeof(reset)) ||
        std::memcmp(reinterpret_cast<void*>(0x45F450), apply, sizeof(apply)) ||
        core::sync::SelectionState::CharacterCell(character) < 0)
        throw std::runtime_error("unsupported_build_or_character");
    DisableDrawing(device);
    core::hooks::TimeHooks::SetSleepBypass(true);
    core::hooks::TimeHooks::SetTimeMultiplier(1000);
    std::fprintf(report, "{\"event\":\"start\",\"schema\":2,\"character\":%u,\"moon\":%u,"
        "\"scope\":\"synthetic_state_frame_steps\",\"controller_input\":false,"
        "\"menu_auto_confirm\":true,\"setup_auto_skip\":true,"
        "\"render_skip\":true,\"pacing_bypass\":true,\"entry_only\":%s,\"max_state_frames\":%u}\n",
        character, moon, entryOnly ? "true" : "false", maxStateFrames);
    Flush();
    initialized = true;
}
}
bool Step(int appMode, IDirect3DDevice9* device) {
    static const wchar_t* directory = _wgetenv(L"CCCASTER_STATE_SWEEP");
    if (!directory || !*directory || appMode != 1 || !domain::scene::SceneFastBoot::IsComplete())
        return false;
    try {
        if (!initialized) Initialize(directory, device);
        auto& mem = GameMem();
        core::SpeedFlags::SetHighSpeed();
        core::timer::FrameTiming::releaseDueTicks = 0;
        core::timer::FrameTiming::presentDueTicks = 0;
        if (!stage) {
            const auto mode = mem.GameMode();
            const auto intro = mem.IntroState();
            if (setupMode != mode || setupIntro != intro) {
                const auto now = platform::RealMonotonicUs();
                if (setupMode == CC_GAME_MODE_LOADING && mode != CC_GAME_MODE_LOADING)
                    loadingUs += now - loadingStarted;
                if (mode == CC_GAME_MODE_LOADING && setupMode != CC_GAME_MODE_LOADING)
                    loadingStarted = now;
                setupMode = mode; setupIntro = intro; skipFrames = 0;
                std::fprintf(report, "{\"event\":\"setup_phase\",\"mode\":%u,\"intro\":%u,"
                    "\"elapsed_us\":%lld}\n", unsigned(mode), unsigned(intro), now - started);
                Flush();
            }
            if (mode == CC_GAME_MODE_CHARA_SELECT) {
                core::sync::SelectionState selection;
                selection.character = character; selection.moon = moon; selection.confirmed = 1;
                mem.DriveRemoteSelection(false, selection, ++nav);
                selection.character = 0; selection.moon = 0;
                mem.DriveRemoteSelection(true, selection, nav);
                *CC_STAGE_SELECTOR_ADDR = 1;
                // キャラ資産ロードは標準の決定経路。戦闘用コマンド・物理パッドは使わない。
                const game_interface::GameInput confirm{0, uint16_t(nav % 8 == 0 ? CC_BUTTON_A | CC_BUTTON_CONFIRM : 0)};
                mem.WriteInput(confirm, confirm);
                return true;
            }
            // 起動時は既存FastBootの決定・イントロスキップを使う。
            // 以後はロード／登場演出だけ8更新ごとに決定を送る。実ロード完了は偽装しない。
            if (mode == CC_GAME_MODE_LOADING || (mode == CC_GAME_MODE_IN_GAME && intro == 2)) {
                game_interface::GameInput skip{};
                if (++skipFrames % 8 == 1) {
                    skip.buttons = CC_BUTTON_A | CC_BUTTON_CONFIRM;
                    if (mode == CC_GAME_MODE_LOADING) ++loadingSkipPulses;
                    else ++introSkipPulses;
                }
                mem.WriteInput(skip, skip);
                return true;
            }
            // pre-game以降は毎更新中立に戻し、戦闘の保存点へ決定入力を持ち越さない。
            mem.WriteInput({}, {});
            if (mode != CC_GAME_MODE_IN_GAME || intro != 0) return true;
            if (!combatReadyUs) combatReadyUs = platform::RealMonotonicUs() - started;
            if (++warmup < 10) return true;
            if (!Read<uint8_t>(0x555130) || !Read<uint8_t>(0x555C2C))
                throw std::runtime_error("actors_not_ready");
            if (Read<uint32_t>(0x74D840) != character || Read<uint32_t>(0x74D84C) != moon)
                throw std::runtime_error("loaded_selection_mismatch");
            Enumerate();
            if (catalogOnly) Finish("catalog");
            const auto size = mem.SnapshotSize();
            if (!size) throw std::runtime_error("snapshot_unavailable");
            baseline.resize(size); saved.resize(size); first.resize(size); second.resize(size); restored.resize(size);
            Save(baseline);
            stage = 1;
            scanStarted = platform::RealMonotonicUs();
            Next();
            return true;
        }
        if (mem.GameMode() != CC_GAME_MODE_IN_GAME || mem.IntroState() != 0)
            throw std::runtime_error("left_combat_boundary");
        if (phase == 1) {
            Save(first); Observe(observed1);
            Load(saved); Save(restored); Observe(observed2);
            WatchDiff(observed0, observed2, "restore");
            if (saved != restored) {
                Dump("saved.bin", saved); Dump("restored.bin", restored);
                throw std::runtime_error("restore_roundtrip_mismatch");
            }
            mem.BeginReplay(simulationFrame, simulationFrame + 1);
            mem.WriteInput({}, {});
            mem.BeginSimulation(simulationFrame);
            phase = 2;
            return true;
        }
        Save(second); Observe(observed2); mem.EndReplay();
        WatchDiff(observed1, observed2, "replay");
        ++checked;
        size_t different = 0, firstOffset = 0;
        for (size_t i = 0; i < first.size(); ++i) if (first[i] != second[i]) {
            if (!different) firstOffset = i;
            ++different;
        }
        std::fprintf(report, "{\"event\":\"result\",\"case\":%zu,\"equal\":%s,"
            "\"different_bytes\":%zu,\"first_offset\":%zu,\"frame\":%u,\"state_age\":%u}\n",
            current, different ? "false" : "true", different, firstOffset, caseFrame, stateAge);
        Flush();
        if (different) {
            ++mismatches;
            Dump("saved.bin", saved); Dump("first.bin", first); Dump("second.bin", second);
            Dump("watch_saved.bin", observed0);
            Dump("watch_first.bin", observed1); Dump("watch_second.bin", observed2);
            Finish("failed", "one_update_mismatch");
        }
        if (stateAge < coveredAges.size() && !coveredAges[stateAge]) {
            coveredAges[stateAge] = true; ++caseCovered; ++coveredFrames;
        }
        ++caseFrame;
        const auto& c = cases[current];
        const auto actor = 0x555134 + c.slot * 0xAFC;
        const bool left = !Read<uint8_t>(actor - 4) || Read<uint32_t>(actor + 0xC) != c.pattern ||
            Read<uint32_t>(actor + 0x10) != c.state || Read<uint32_t>(actor + 0x31C) != c.animation;
        const bool complete = caseCovered == coveredAges.size();
        if (entryOnly || complete || left || caseFrame >= maxStateFrames) {
            std::fprintf(report, "{\"event\":\"end_case\",\"case\":%zu,\"frames\":%u,"
                "\"covered_frame_points\":%llu,\"nominal_frame_points\":%zu,\"reason\":\"%s\"}\n",
                current, caseFrame, caseCovered, coveredAges.size(),
                entryOnly ? "entry_only" : complete ? "nominal_complete" : left ? "state_changed_early" : "frame_limit");
            ++endedCases;
            ++current;
            Next();
        } else BeginFrame();
    } catch (const std::exception& error) {
        Finish("failed", error.what());
    }
    return true;
}
}
