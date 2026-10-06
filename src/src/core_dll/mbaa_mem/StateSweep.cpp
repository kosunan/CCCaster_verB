#include "core_dll/mbaa_mem/StateSweep.hpp"
#include "core_dll/mbaa_mem/IGameMemory.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"
#include "core_dll/mbaa_mem/GameBuildGuard.hpp"
#include "core_dll/engine/SceneFastBoot.hpp"
#include "core_dll/engine/TrainingCharacterSelection.hpp"
#include "core_dll/timing/SpeedFlags.hpp"
#include "core_dll/timing/FrameTiming.hpp"
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
#include <unordered_set>
#include <vector>

// 0x45F2D0: EAX=ActorData。0x45F450: cdecl(ActorData*)。
// 0x426940/0x426946にも同じ順の初期化がある。ABIは命令列で確認。
extern "C" __attribute__((naked)) void cc_sweep_reset_actor(void*) {
    __asm__ __volatile__("movl 4(%esp),%eax; movl $0x45F2D0,%edx; jmp *%edx");
}
// 0x46DB40から呼ばれるモーション更新。入口のEAX以外はcallee-saveで保全される。
extern "C" __attribute__((naked)) void cc_sweep_step_actor(void*) {
    __asm__ __volatile__("movl 4(%esp),%eax; movl $0x4618C0,%edx; jmp *%edx");
}
extern "C" void cc_training_describe(void*, void*);
extern "C" void cc_training_free_slot(unsigned);
extern "C" bool cc_training_file_exists(const char*);

namespace cccaster::testing::state_sweep {
namespace {
using game_interface::GameMem;
struct Case { uint32_t slot, pattern, state, duration, animation; };
struct Range { uintptr_t address; size_t length; };
// 保存表から生成しない独立した監視候補。差を即保存漏れと断定せず、生の候補として報告。
constexpr Range watch[] = {{0x55512C,4}, {0x5581D0,1048}, {0x558600,8},
                           {0x55D1CC,4}, {0x56406C,4}, {0x76E008,1500},
                           {0x564AFC,4}, {0x564B04,8}, {0x564B20,4},
                           {0x562A50,4}, {0x563948,228}, {0x563868,224}, {0x5595BC,4}};
FILE* report = nullptr;
std::filesystem::path output;
std::vector<Case> cases;
std::vector<char> baseline, saved, first, second, restored, observed0, observed1, observed2;
std::vector<char> globalsSaved, globalsFirst, globalsSecond;
size_t current = 0;
uint32_t character = 0, moon = 0, nav = 0, stage = 0, warmup = 0;
int phase = 0;
int patternFilter = -1, stateFilter = -1;
bool initialized = false, catalogOnly = false;
bool entryOnly = false;
bool fixtures = false;
bool holdFixtures = false;
bool primarySideOnly = false;
bool extraLoaded = false;
uint32_t attempt = 0, attemptFrame = 0, prepareFrame = 0;
uint32_t stalledFrames = 0, lastAge = UINT32_MAX;
std::unordered_set<uint64_t> preparationPoints;
uint32_t preparationStalled = 0;
game_interface::GameInput holdInput{};
Case threat{};
bool haveThreat = false;
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
LONG WINAPI RecordNativeException(EXCEPTION_POINTERS* exception) {
    if (report && exception && exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        const auto* r = exception->ExceptionRecord;
        const auto* c = exception->ContextRecord;
        std::fprintf(report, "{\"event\":\"native_exception\",\"case\":%zu,\"attempt\":%u,"
            "\"eip\":%lu,\"address\":%lu,\"eax\":%lu,\"ecx\":%lu,\"edx\":%lu,\"esi\":%lu,\"edi\":%lu}\n",
            current, attempt, c->Eip, r->NumberParameters > 1 ? DWORD(r->ExceptionInformation[1]) : 0,
            c->Eax, c->Ecx, c->Edx, c->Esi, c->Edi);
        Flush();
        if (!saved.empty()) {
            auto* file = _wfopen((output / "crash_saved.bin").c_str(), L"wb");
            if (file) { std::fwrite(saved.data(), 1, saved.size(), file); std::fclose(file); }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
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
void ObserveDiagnosticGlobals(std::vector<char>& bytes) {
    if (patternFilter < 0 || stateFilter < 0) return;
    bytes.resize(0x1A000);
    SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(0x54B000), bytes.data(), bytes.size(), &read)
        || read != bytes.size()) throw std::runtime_error("diagnostic_read_failed");
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
void ReportCommands(uintptr_t animation) {
    for (unsigned group = 0; group < 2; ++group) {
        const auto count = Read<uint8_t>(animation+0x40+group);
        const auto table = Read<uint32_t>(animation+0x44+group*4);
        std::fprintf(report, ",\"%s\":[", group ? "entry_effects" : "effects");
        for (unsigned i = 0; i < count; ++i) {
            const auto ef = Read<uint32_t>(table+i*4);
            if (!ef) { std::fprintf(report, "%snull", i ? "," : ""); continue; }
            std::fprintf(report, "%s[", i ? "," : "");
            // 毎F命令は9語、入口命令は0x38B（0x454E30の生成サイズ）。
            for (unsigned j = 0; j < (group ? 14U : 9U); ++j)
                std::fprintf(report, "%s%d", j ? "," : "", Read<int32_t>(ef+j*4));
            std::fprintf(report, "]");
        }
        std::fprintf(report, "]");
    }
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
                if (!haveThreat && slot == 1 && pattern >= 50 && pattern < 100) {
                    const auto attack = Read<uint32_t>(animation+0x3C);
                    const auto stateData = Read<uint32_t>(animation+0x38);
                    if (attack && !(Read<uint32_t>(attack) & 0x4000) && Read<uint8_t>(stateData+0x10)) {
                        threat = {slot, pattern, state, duration, animation};
                        haveThreat = true;
                    }
                }
                ++definitionCount;
                const bool selected = (!primarySideOnly || slot % 2 == 0) &&
                                      (patternFilter < 0 || pattern == unsigned(patternFilter)) &&
                                      (stateFilter < 0 || state == unsigned(stateFilter));
                std::fprintf(report, "{\"event\":\"definition\",\"slot\":%u,\"actor_character\":%u,"
                    "\"actor_moon\":%u,\"pattern\":%u,\"state\":%u,\"duration\":%u,\"animation_type\":%u,"
                    "\"selected\":%s}\n", slot, unsigned(Read<uint8_t>(player+5)),
                    unsigned(Read<uint16_t>(player+12)), pattern, state, unsigned(duration),
                    unsigned(Read<uint8_t>(animation+14)), selected ? "true" : "false");
                if (patternFilter >= 0 && slot%2 == 0) {
                    std::fprintf(report, "{\"event\":\"motion_data\",\"slot\":%u,\"pattern\":%u,\"state\":%u",
                        slot, pattern, state);
                    ReportCommands(animation);
                    std::fprintf(report, "}\n");
                }
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
void WriteCaseInput() {
    auto input = holdInput;
    if (Read<uint8_t>(0x555134 + cases[current].slot*0xAFC + 0x310)) {
        if (input.direction == 4 || input.direction == 6) input.direction = 10 - input.direction;
        else if (input.direction == 7 || input.direction == 9) input.direction = 16 - input.direction;
        else if (input.direction == 1 || input.direction == 3) input.direction = 4 - input.direction;
    }
    if (cases[current].slot % 2 == 0) GameMem().WriteInput(input, {});
    else GameMem().WriteInput({}, input);
}
void BeginFrame() {
    const auto& selected = cases[current];
    const auto selectedActor = 0x555134 + selected.slot*0xAFC;
    if (!Read<uint8_t>(selectedActor-4) || Read<uint32_t>(selectedActor+0xC) != selected.pattern ||
        Read<uint32_t>(selectedActor+0x10) != selected.state || Read<uint32_t>(selectedActor+0x31C) != selected.animation)
        throw std::runtime_error("frame_identity_mismatch");
    if (attempt == 74 || attempt == 75) {
        const auto actor = 0x555134 + cases[current].slot*0xAFC;
        const auto enemy = 0x555134 + (cases[current].slot%2 ^ 1)*0xAFC;
        // 接触・投げ成立を避ける相手位置。各保存点にはこの前提を含める。
        const int direction = Read<uint8_t>(actor+0x310) ? -1 : 1;
        Write<int32_t>(enemy+0x104, Read<int32_t>(actor+0x104) - direction*200000);
        Write<int32_t>(enemy+0x110, Read<int32_t>(enemy+0x104));
    }
    if (attempt == 40 || attempt == 41) {
        if (!haveThreat) throw std::runtime_error("guard_threat_unavailable");
        const auto actor = 0x555134 + cases[current].slot*0xAFC;
        const auto enemy = 0x555134 + threat.slot*0xAFC;
        const int direction = Read<uint8_t>(actor+0x310) ? -1 : 1;
        // 近接ガード範囲100px内、通常小技の実当たりの外側に毎F攻撃を用意する。
        // 検査対象の経過Fは触らず、同じ相手状態を含む保存点からA/Bを比較する。
        Write<int32_t>(enemy+0x104, Read<int32_t>(actor+0x104) + direction*25200);
        Write<int32_t>(enemy+0x110, Read<int32_t>(enemy+0x104));
        Write<uint8_t>(enemy+0x310, direction == 1);
        Write<uint8_t>(enemy+0x311, direction == 1);
        Write<uint32_t>(enemy+0xC, threat.pattern);
        Write<uint32_t>(enemy+0x10, threat.state);
        cc_sweep_reset_actor(reinterpret_cast<void*>(enemy));
        reinterpret_cast<int (__cdecl*)(void*)>(0x45F450)(reinterpret_cast<void*>(enemy));
    }
    WriteCaseInput();
    stateAge = Read<uint32_t>(0x555134 + cases[current].slot * 0xAFC + 0x18);
    std::fprintf(report, "{\"event\":\"frame_begin\",\"case\":%zu,\"frame\":%u,\"state_age\":%u,\"attempt\":%u,\"frame_source\":\"%s\"}\n",
                 current, caseFrame, stateAge, attempt, attempt >= 106 ? "native_actor_setup" : "native_world_update");
    Flush();
    Save(saved); Observe(observed0); ObserveDiagnosticGlobals(globalsSaved);
    simulationFrame = 65537 + uint32_t(checked);
    GameMem().BeginSimulation(simulationFrame);
    phase = 1;
}
uintptr_t PreparePartner(uint32_t slot) {
    const auto actor = 0x555134 + slot*0xAFC;
    const auto partner = 0x555134 + (slot^2)*0xAFC;
    if (!Read<uint8_t>(partner-4)) {
        std::memcpy(reinterpret_cast<void*>(partner-4), reinterpret_cast<void*>(actor-4), 0xAFC);
        Write<uint8_t>(partner, slot^2);
        Write<uint32_t>(partner+0x324, partner-4);
        Write<uint32_t>(partner+0x2C8, partner);
        Write<uint32_t>(partner+0xC, 0); Write<uint32_t>(partner+0x10, 0);
        cc_sweep_reset_actor(reinterpret_cast<void*>(partner));
        reinterpret_cast<int (__cdecl*)(void*)>(0x45F450)(reinterpret_cast<void*>(partner));
    }
    Write<uint32_t>(actor+0x328, partner); Write<uint32_t>(partner+0x328, actor);
    return partner;
}
void ApplyEntry() {
    const auto& c = cases[current];
    const auto actor = 0x555134 + c.slot * 0xAFC;
    const bool precursor = attempt == 2 || attempt == 3 || (attempt >= 42 && attempt < 68);
    bool tagState = false;
    const auto effectCount = Read<uint8_t>(c.animation+0x40);
    const auto effectTable = Read<uint32_t>(c.animation+0x44);
    for (unsigned i = 0; i < effectCount; ++i) {
        const auto ef = Read<uint32_t>(effectTable+i*4);
        if (ef && Read<uint32_t>(ef) == 100) tagState = true;
    }
    if (tagState) {
        // EF100はpartnerPtrを必須とし、制御側では状態時計を0へ戻す交代待ち命令。
        // 単独キャラの共有定義にもあるため、同じ資産の交代相手と従属側を用意する。
        const auto partner = PreparePartner(c.slot);
        Write<uint8_t>(actor+0x174, 1); Write<uint8_t>(partner+0x174, 0);
        Write<uint32_t>(0x557DB8 + (c.slot%2)*0x20C, c.slot^2);
    }
    // ガードは直前の攻撃による近接ガード猶予が必要（0x4666B0で毎更新減算）。
    if (attempt >= 4 && (c.pattern == 17 || c.pattern == 18))
        Write<uint8_t>(actor + 0x179, 8);
    // EF25の比較対象を境界の両側から試す。経過Fや定義自体は書き換えない。
    if (attempt >= 30 && attempt < 34) {
        const auto count = Read<uint8_t>(c.animation + 0x40);
        const auto table = Read<uint32_t>(c.animation + 0x44);
        for (unsigned i = 0; i < count; ++i) {
            const auto ef = Read<uint32_t>(table+i*4);
            if (!ef) continue; // 0x4684C0と同じく、欠番の命令を飛ばす。
            if (Read<uint32_t>(ef) == 25 && Read<uint32_t>(ef+8) < 10) {
                const auto threshold = Read<int32_t>(ef+12);
                const auto value = std::clamp<int64_t>(int64_t(threshold) + (attempt < 32 ? 1 : -1), -32768, 32767);
                Write<int16_t>(actor + 0x1C0 + Read<uint32_t>(ef+8)*2, int16_t(value));
            }
        }
    }
    Write<uint32_t>(actor + 0xC, c.pattern);
    Write<uint32_t>(actor + 0x10, precursor ? 0 : c.state);
    cc_sweep_reset_actor(reinterpret_cast<void*>(actor));
    reinterpret_cast<int (__cdecl*)(void*)>(0x45F450)(reinterpret_cast<void*>(actor));
    if (attempt == 104 || attempt == 105) {
        // EF36のヒート状態分岐。メーターと残量を持つ継続条件を入口で用意する。
        for (unsigned i = 0; i < effectCount; ++i) {
            const auto ef = Read<uint32_t>(effectTable+i*4);
            if (ef && Read<uint32_t>(ef) == 36) {
                const auto expected = Read<uint32_t>(ef+8);
                const auto state = Read<uint32_t>(ef+12) ? expected : (expected+1)%4;
                Write<uint8_t>(actor+0xE4, uint8_t(state));
                Write<uint32_t>(actor+0xDC, 30000);
                Write<uint16_t>(actor+0xE8, 65535);
                Write<uint16_t>(actor+0xEA, 65535);
            }
        }
    }
    if (attempt == 70 || attempt == 71) {
        // 被弾中にも残る技・子オブジェクトのEF14条件を検査する。
        Write<int32_t>(actor+0x1A8, std::max(1U, c.duration));
    }
    if (attempt >= 76 && attempt < 80) {
        Write<int32_t>(actor+0x168, 0);
        Write<int32_t>(actor+0xDC, 30000);
    }
    if (attempt == 78 || attempt == 79) {
        // 0x4618C0: 移動許可付きヒットストップでは状態時計が進み、盾の保持時計は止まる。
        Write<uint8_t>(actor+0x16E, 2);
        Write<uint8_t>(actor+0x16F, 1);
    }
    if (attempt == 108 || attempt == 109) {
        // モーション更新を許すヒットストップ中は盾時計が進まない（0x468290）。
        // 定義上の長い盾待機も、通常更新で途中解放されるFとは分けて構成・比較する。
        const auto frames = uint8_t(std::min(255U, c.duration+1));
        Write<uint8_t>(actor+0x16E, frames);
        Write<uint8_t>(actor+0x16F, frames);
    }
    if (attempt == 102 || attempt == 103) {
        // EF11（0x45D700）の暗転停止。発動者は更新され、停止対象の子は通知を保留する。
        const auto owner = Read<uint8_t>(actor);
        Write<uint32_t>(0x558908 + owner*0x30C, std::max(1U, c.duration)+4);
        Write<uint32_t>(0x558910 + owner*0x30C, Read<uint32_t>(0x558910 + owner*0x30C)+1);
        Write<uint32_t>(0x5595BC, 1);
    }
    if (attempt == 36 || attempt == 37) {
        // 0x423900で終了するスローの最終F。前段EFと後段更新の境界を実処理で通す。
        Write<uint16_t>(0x55D208, 1);
    }
    if (Read<uint32_t>(actor+0xC) != c.pattern || Read<uint32_t>(actor+0x10) != (precursor ? 0 : c.state))
        throw std::runtime_error("entry_rejected_or_redirected");
    if (Read<uint32_t>(actor+0x10) != c.state) {
        WriteCaseInput();
        phase = 3;
        return;
    }
    if (Read<uint32_t>(actor+0x31C) != c.animation) throw std::runtime_error("entry_animation_mismatch");
    BeginFrame();
}
void StartAttempt() {
    const auto& c = cases[current];
    Load(baseline);
    if (attempt >= 80 && attempt < 96) {
        // 同じ基準点からゲーム自身の乱数を進め、確率分岐の別の履歴を用意する。
        for (unsigned i = 0; i < (attempt-79)*13; ++i)
            reinterpret_cast<uint32_t (__cdecl*)()>(0x421A80)();
    }
    attemptFrame = 0; prepareFrame = 0; stalledFrames = 0; lastAge = UINT32_MAX;
    preparationPoints.clear(); preparationStalled = 0;
    const auto actor = 0x555134 + c.slot * 0xAFC;
    holdInput = {};
    const bool precursor = attempt == 2 || attempt == 3 || (attempt >= 42 && attempt < 68);
    const bool air = attempt == 1 || attempt == 3 || (attempt >= 4 && attempt < 80 && attempt % 2 != 0 && attempt != 41) || attempt == 38 || attempt == 72 || (attempt >= 97 && attempt%2 != 0);
    if ((attempt >= 4 && attempt < 30) || (attempt >= 42 && attempt < 68)) {
        constexpr game_interface::GameInput holds[] = {{2,0},{6,0},{4,0},{0,CC_BUTTON_A},
            {0,CC_BUTTON_B},{0,CC_BUTTON_C},{0,CC_BUTTON_D},{0,CC_BUTTON_AB},{0,CC_BUTTON_E},
            {1,0},{7,0},{8,0},{9,0}};
        holdInput = holds[(attempt - (attempt >= 42 ? 42 : 4)) / 2];
    }
    if (attempt == 40 || attempt == 41) {
        holdInput.direction = attempt == 40 ? 4 : 1;
    }
    if (attempt >= 76 && attempt < 80) holdInput.buttons = CC_BUTTON_D;
    if (attempt == 96 || attempt == 97) holdInput.direction = 3;
    if (attempt >= 98 && attempt < 102) {
        const auto partner = PreparePartner(c.slot);
        Write<uint8_t>(actor+0x174, 2); Write<uint8_t>(partner+0x174, 0);
        Write<uint32_t>(0x557DB8 + (c.slot%2)*0x20C, c.slot^2);
        if (attempt >= 100) {
            // 0x46DB40の同時制御モード。補助側の追従処理を使わず両者を通常更新する。
            Write<uint32_t>(0x562A74, Read<uint32_t>(0x562A74) | 0x200);
            Write<uint8_t>(actor+0x174, 0);
        }
    }
    if (attempt == 68 || attempt == 69) {
        // 0x466200の交代完了と同じ制御側・従属側の組合せを用意する。
        const auto partner = 0x555134 + (c.slot ^ 2)*0xAFC;
        Write<uint32_t>(0x557DB8 + (c.slot%2)*0x20C, c.slot);
        Write<uint8_t>(actor+0x173, 0); Write<uint8_t>(actor+0x174, 0);
        Write<uint16_t>(actor+0x1F0, 0);
        Write<uint8_t>(partner+0x174, 1); Write<uint16_t>(partner+0x1F0, 1);
        Write<uint32_t>(actor+0x328, partner);
    }
    std::fprintf(report, "{\"event\":\"attempt\",\"case\":%zu,\"attempt\":%u,\"air\":%s,"
        "\"precursor\":%s,\"direction\":%u,\"buttons\":%u}\n", current, attempt,
        air ? "true" : "false", precursor ? "true" : "false", holdInput.direction, holdInput.buttons);
    Flush();
    // 0x4620B0の着地判定に必要な高さを入口で作り、以後の位置・速度はゲーム自身が進める。
    if (air) {
        const int height = attempt == 72 || attempt == 73 ? -20000000 :
                           attempt == 38 || attempt == 39 ? -1048576 : -131072;
        Write<int32_t>(actor + 0x108, height);
        Write<int32_t>(actor + 0x114, height);
        Write<uint8_t>(actor + 0x315, 0);
    }
    if (attempt >= 4) {
        // 入力の派生状態を書かず、通常の読取を2更新通して保持済みの前提を作る。
        WriteCaseInput();
        phase = 4;
        return;
    }
    ApplyEntry();
}
void Next() {
    if (current == cases.size()) Finish("complete");
    const auto& c = cases[current];
    // 先にチェックポイントを永続化。ネイティブ関数の例外・ハングも未検査と区別する。
    std::fprintf(report, "{\"event\":\"begin_case\",\"case\":%zu,\"slot\":%u,\"pattern\":%u,\"state\":%u}\n",
                 current, c.slot, c.pattern, c.state);
    caseFrame = 0; caseCovered = 0; attempt = 0;
    coveredAges.assign(std::max(1U, c.duration), false);
    StartAttempt();
}
void EndAttempt(const char* reason) {
    const auto actor = 0x555134 + cases[current].slot * 0xAFC;
    std::fprintf(report, "{\"event\":\"end_attempt\",\"case\":%zu,\"attempt\":%u,\"reason\":\"%s\","
        "\"pattern\":%u,\"state\":%u,\"age\":%u,\"y\":%d,\"landing\":%u,\"prepared_frames\":%u,"
        "\"shield_clock\":%d,\"hitstun\":%d,\"guard_clock\":%u,\"puppet\":%u,\"extra9\":%d}\n",
        current, attempt, reason, Read<uint32_t>(actor+0xC), Read<uint32_t>(actor+0x10),
        Read<uint32_t>(actor+0x18), Read<int32_t>(actor+0x108), unsigned(Read<uint8_t>(actor+0x315)), prepareFrame,
        Read<int32_t>(actor+0x168), Read<int32_t>(actor+0x1A8), unsigned(Read<uint8_t>(actor+0x179)),
        unsigned(Read<uint8_t>(actor+0x174)), int(Read<int16_t>(actor+0x1D2)));
    const bool complete = caseCovered == coveredAges.size();
    const bool moreAttempts = holdFixtures ? attempt != 109 : attempt < 3;
    if (!entryOnly && !complete && fixtures && moreAttempts) {
        // 子の終了待ちは暗転前提を先に試し、長い同一経路の反復を減らす。
        attempt = holdFixtures && attempt == 29 ? 102 : holdFixtures && attempt == 103 ? 30 :
                  holdFixtures && attempt == 101 ? 104 : attempt+1;
        StartAttempt();
        return;
    }
    std::fprintf(report, "{\"event\":\"end_case\",\"case\":%zu,\"frames\":%u,"
        "\"covered_frame_points\":%llu,\"nominal_frame_points\":%zu,\"reason\":\"%s\"}\n",
        current, caseFrame, caseCovered, coveredAges.size(), complete ? "nominal_complete" : reason);
    if (!complete) {
        const auto anim = cases[current].animation;
        std::fprintf(report, "{\"event\":\"gap_conditions\",\"case\":%zu", current);
        ReportCommands(anim);
        std::fprintf(report, "}\n");
    }
    ++endedCases;
    ++current;
    Next();
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
    AddVectoredExceptionHandler(1, RecordNativeException);
    character = Option("CCCASTER_SWEEP_CHARACTER", 0, 0, 100);
    moon = Option("CCCASTER_SWEEP_MOON", 0, 0, 9);
    patternFilter = Option("CCCASTER_SWEEP_PATTERN", -1, 0, 9999);
    stateFilter = Option("CCCASTER_SWEEP_STATE", -1, 0, 9999);
    catalogOnly = Option("CCCASTER_SWEEP_CATALOG", 0, 0, 1) != 0;
    entryOnly = Option("CCCASTER_SWEEP_ENTRY_ONLY", 0, 0, 1) != 0;
    fixtures = Option("CCCASTER_SWEEP_FIXTURES", 0, 0, 1) != 0;
    holdFixtures = Option("CCCASTER_SWEEP_HOLD_FIXTURES", 0, 0, 1) != 0;
    primarySideOnly = Option("CCCASTER_SWEEP_PRIMARY_SIDE_ONLY", 0, 0, 1) != 0;
    maxStateFrames = Option("CCCASTER_SWEEP_MAX_STATE_FRAMES", 600, 1, 100000);
    constexpr unsigned char reset[] = {0x53,0x33,0xdb,0x56,0x8b,0xf0,0x57};
    constexpr unsigned char apply[] = {0x55,0x8b,0xec,0x83,0xe4,0xf8,0x83,0xec,0x0c};
    if (!game_build::RuntimeValidated() ||
        std::memcmp(reinterpret_cast<void*>(0x45F2D0), reset, sizeof(reset)) ||
        std::memcmp(reinterpret_cast<void*>(0x45F450), apply, sizeof(apply)) ||
        std::find(training_character::Characters.begin(), training_character::Characters.end(), character) == training_character::Characters.end())
        throw std::runtime_error("unsupported_build_or_character");
    DisableDrawing(device);
    // 本体のフレーム待機は起動時にバイパス済み。状態走査ではツール側も待たない。
    std::fprintf(report, "{\"event\":\"start\",\"schema\":2,\"character\":%u,\"moon\":%u,"
        "\"scope\":\"synthetic_state_frame_steps\",\"controller_input\":false,"
        "\"menu_auto_confirm\":true,\"setup_auto_skip\":true,"
        "\"render_skip\":true,\"pacing_bypass\":true,\"entry_only\":%s,\"max_state_frames\":%u,"
        "\"fixtures\":%s,\"scripted_hold_input\":%s,\"primary_side_only\":%s}\n",
        character, moon, entryOnly ? "true" : "false", maxStateFrames,
        fixtures ? "true" : "false", holdFixtures ? "true" : "false", primarySideOnly ? "true" : "false");
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
                selection.character = training_character::CursorCharacter(character);
                selection.moon = moon < 3 ? moon : 0; selection.confirmed = 1;
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
            if (!extraLoaded && (moon >= 3 || core::sync::SelectionState::CharacterCell(character) < 0)) {
                // 標準キャラセレにない資産は、Trainingのキャラ変更と同じネイティブ読込で準備。
                // 描画・メニューのムーン配列へ専用値8/9を流さず、実在する定義だけを受け付ける。
                const auto descriptor = Item(Read<uint32_t>(0x55DF18), character, 10000);
                for (unsigned field : {0x34U, 0x54U}) {
                    const auto* name = reinterpret_cast<const char*>(descriptor+field);
                    if (field == 0x54 && (!*name || *name == '0')) continue;
                    char path[128]{};
                    std::snprintf(path, sizeof(path), ".\\data\\%.31s_%u.txt", name, moon);
                    const auto attr = GetFileAttributesA(path);
                    if ((attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) && !cc_training_file_exists(path))
                        throw std::runtime_error("character_style_asset_missing");
                }
                Write<uint32_t>(0x74D840, character); Write<uint32_t>(0x74D84C, moon);
                Write<uint32_t>(0x74D85C, 0); Write<uint32_t>(0x74D860, 0);
                uint32_t description[0xB8/4]{}, loading[3]{};
                cc_training_describe(description, loading);
                for (unsigned slot = 0; slot < 4; ++slot) cc_training_free_slot(slot);
                reinterpret_cast<int (__fastcall*)(void*, void*)>(0x4489E0)(description, nullptr);
                // 0x423570が次更新で正規battleオブジェクトを渡して0x423380を呼ぶ。
                Write<uint8_t>(0x55DEC3, 1);
                extraLoaded = true; warmup = 0;
                std::fprintf(report, "{\"event\":\"extra_assets_loaded\",\"character\":%u,\"moon\":%u}\n", character, moon);
                Flush();
                return true;
            }
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
        if (phase == 4) {
            if (++prepareFrame >= 2 && ((attempt != 36 && attempt != 37) || mem.RealTimer()%3 != 0)) {
                if (attempt == 34 || attempt == 35 || attempt == 39) {
                    const auto actor = 0x555134 + cases[current].slot*0xAFC;
                    const auto enemy = 0x555134 + (cases[current].slot%2 ^ 1)*0xAFC;
                    Write<int32_t>(actor+0x104, 0); Write<int32_t>(actor+0x110, 0);
                    Write<uint8_t>(actor+0x310, 0); Write<uint8_t>(actor+0x311, 0);
                    Write<int32_t>(enemy+0x104, -50000); Write<int32_t>(enemy+0x110, -50000);
                }
                ApplyEntry();
            }
            else WriteCaseInput();
            return true;
        }
        if (phase == 3) {
            const auto& c = cases[current];
            const auto actor = 0x555134 + c.slot * 0xAFC;
            ++prepareFrame;
            const uint64_t point = (uint64_t(Read<uint32_t>(actor+0x10)) << 32) | Read<uint32_t>(actor+0x18);
            preparationStalled = preparationPoints.insert(point).second ? 0 : preparationStalled+1;
            if (Read<uint32_t>(actor+0xC) == c.pattern && Read<uint32_t>(actor+0x10) == c.state &&
                Read<uint32_t>(actor+0x31C) == c.animation) BeginFrame();
            else if (Read<uint32_t>(actor+0xC) != c.pattern || prepareFrame >= maxStateFrames || preparationStalled >= 512)
                EndAttempt("precondition_not_reached");
            else WriteCaseInput();
            return true;
        }
        if (phase == 1) {
            Save(first); Observe(observed1); ObserveDiagnosticGlobals(globalsFirst);
            Load(saved); Save(restored); Observe(observed2);
            WatchDiff(observed0, observed2, "restore");
            if (saved != restored) {
                Dump("saved.bin", saved); Dump("restored.bin", restored);
                throw std::runtime_error("restore_roundtrip_mismatch");
            }
            mem.BeginReplay(simulationFrame, simulationFrame + 1);
            WriteCaseInput();
            mem.BeginSimulation(simulationFrame);
            phase = 2;
            return true;
        }
        Save(second); Observe(observed2); ObserveDiagnosticGlobals(globalsSecond); mem.EndReplay();
        WatchDiff(observed1, observed2, "replay");
        ++checked;
        size_t different = 0, firstOffset = 0;
        if (std::memcmp(first.data(), second.data(), first.size()))
        for (size_t i = 0; i < first.size(); ++i) if (first[i] != second[i]) {
            if (!different) firstOffset = i;
            ++different;
        }
        std::fprintf(report, "{\"event\":\"result\",\"case\":%zu,\"equal\":%s,"
            "\"different_bytes\":%zu,\"first_offset\":%zu,\"frame\":%u,\"state_age\":%u,\"attempt\":%u,\"frame_source\":\"%s\"}\n",
            current, different ? "false" : "true", different, firstOffset, caseFrame, stateAge, attempt,
            attempt >= 106 ? "native_actor_setup" : "native_world_update");
        Flush();
        if (different) {
            ++mismatches;
            Dump("saved.bin", saved); Dump("first.bin", first); Dump("second.bin", second);
            Dump("watch_saved.bin", observed0);
            Dump("watch_first.bin", observed1); Dump("watch_second.bin", observed2);
            if (!globalsSaved.empty()) {
                Dump("globals_54B000_saved.bin", globalsSaved);
                Dump("globals_54B000_first.bin", globalsFirst); Dump("globals_54B000_second.bin", globalsSecond);
            }
            Finish("failed", "one_update_mismatch");
        }
        if (stateAge < coveredAges.size() && !coveredAges[stateAge]) {
            coveredAges[stateAge] = true; ++caseCovered; ++coveredFrames;
        }
        ++caseFrame;
        ++attemptFrame;
        // 同じageへの停止だけでなく、0..Nを循環して先へ進まない経路も打ち切る。
        // 試行ごとの最大到達ageを使い、別試行ですでに検査したFでの早期終了は避ける。
        if (lastAge == UINT32_MAX || stateAge > lastAge) { lastAge = stateAge; stalledFrames = 0; }
        else ++stalledFrames;
        const auto& c = cases[current];
        const auto actor = 0x555134 + c.slot * 0xAFC;
        if (attempt >= 106 && caseCovered != coveredAges.size()) {
            // 無条件分岐等で通常更新から到達しない定義Fを、内部のモーション更新で構成。
            // A/Bで比べる処理は常に通常のゲーム全体1更新。検査対象のEFは省略しない。
            // 構成だけ保存点へ戻して実行し、経過Fの直接設定も行わない。
            Load(saved);
            cc_sweep_step_actor(reinterpret_cast<void*>(actor));
        }
        const bool left = !Read<uint8_t>(actor - 4) || Read<uint32_t>(actor + 0xC) != c.pattern ||
            Read<uint32_t>(actor + 0x10) != c.state || Read<uint32_t>(actor + 0x31C) != c.animation;
        const bool complete = caseCovered == coveredAges.size();
        if (entryOnly || complete || left || attemptFrame >= maxStateFrames || stalledFrames >= 512) {
            EndAttempt(entryOnly ? "entry_only" : complete ? "nominal_complete" : left ? "state_changed_early" :
                       stalledFrames >= 512 ? "state_clock_stalled" : "frame_limit");
        } else BeginFrame();
    } catch (const std::exception& error) {
        Finish("failed", error.what());
    }
    return true;
}
}
