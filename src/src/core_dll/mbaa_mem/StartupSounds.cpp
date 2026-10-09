#include "StartupSounds.hpp"
#include "core_dll/mbaa_mem/StartupPatch.hpp"
#include "core_dll/common/StartupTrace.hpp"
#include "shared_contracts/ProcessMemory.hpp"
#include "core_dll/hook/HookBatch.hpp"
#include "core_dll/rollback/ReplayEffects.hpp"
#include <cstdio>
#include <cstdlib>
namespace {
bool active=false, deferred=false;
std::array<bool,200> attempted{};
constexpr std::array<uint8_t,5> original{0xE8,0x29,0xF9,0x0B,0};
constexpr std::array<uint8_t,5> skip{0x90,0x90,0x90,0x90,0x90};
// 4DDA10の1音分と同じABI。ゲーム自身のローダー・名前登録・設定音量を使う。
__attribute__((naked,cdecl)) void Load(uint32_t, const char*, const char*) {
    __asm__ __volatile__(
        "pushl %esi\n\tpushl %edi\n\t"
        "movl 12(%esp),%esi\n\tmovl 16(%esp),%eax\n\t"
        "pushl %esi\n\tpushl %eax\n\tmovl $0x4DD8C0,%eax\n\tcall *%eax\n\taddl $8,%esp\n\t"
        "testl %eax,%eax\n\tjz 1f\n\t"
        "movl 20(%esp),%edi\n\tmovl $0x4DD560,%eax\n\tcall *%eax\n\t"
        "movl 0x76E648,%ecx\n\tpushl $0\n\tmovl %esi,%eax\n\tmovl $0x4DE100,%edx\n\tcall *%edx\n\taddl $4,%esp\n\t"
        "1: popl %edi\n\tpopl %esi\n\tret\n\t");
}
uint64_t Hash(uintptr_t address,size_t size) {
    uint64_t hash=14695981039346656037ull;
    for(size_t i=0;i<size;++i)hash=(hash^reinterpret_cast<const uint8_t*>(address)[i])*1099511628211ull;
    return hash;
}
}
namespace cccaster::game_memory::startup_sounds {
bool Active(){return active;}
void Initialize(uint8_t mode) {
    if(mode>1 || !diagnostics::startup::HasGate() || diagnostics::startup::Baseline() ||
       std::getenv("CCCASTER_STARTUP_MINIMAL_BASELINE") || !startup::MatchesMenuCode())return;
    if(Hash(0x4DDA10,224)!=0x0830c47cf7251fd0ull ||
       Hash(0x4DD8C0,328)!=0x0124ebde12ab95c1ull ||
       Hash(0x4DD560,89)!=0x1ffacc219daca275ull ||
       Hash(0x4DE100,94)!=0x4938a0dd26d167e9ull)return;
    // キャラ選択のSEも初回要求から再生する。既存の再計算抑止と同じ再生入口を使う。
    hook_batch::Scope hooks(true);
    if(!sync::InstallReplayEffects())return;
    const patch::Spec spec{"startup_common_sounds",0x41E0E2,original,skip};
    const auto result=patch::Apply(std::span(&spec,1));
    if(result.rollbackFailed)ExitProcess(ERROR_WRITE_FAULT);
    active=deferred=bool(result);
    domain::session::DebugLog("[StartupSounds] deferred=%u count=200",unsigned(active));
}
void Ensure(uint32_t sound) {
    if(!deferred || sound>=attempted.size() || attempted[sound])return;
    if(!*reinterpret_cast<const uint32_t*>(0x76E000))return;
    attempted[sound]=true;
    if(reinterpret_cast<const uint32_t*>(0x76C6F8)[sound])return;
    char name[16],path[64];
    std::snprintf(name,sizeof(name),"SE%03u",sound);
    std::snprintf(path,sizeof(path),".\\se\\normal_se\\%s",name);
    Load(sound,path,name);
    if(diagnostics::startup::Enabled())domain::session::DebugLog("[StartupSounds] loaded=%u sound=%u",
        unsigned(reinterpret_cast<const uint32_t*>(0x76C6F8)[sound]!=0),sound);
}
void PrepareBattle() {
    if(!deferred)return;
    unsigned early=0;for(bool value:attempted)early+=value;
    const auto begin=diagnostics::startup::QpcUs();
    for(uint32_t sound=0;sound<attempted.size();++sound)Ensure(sound);
    deferred=false;
    const patch::Spec spec{"startup_common_sounds_restore",0x41E0E2,skip,original};
    if(!patch::Apply(std::span(&spec,1)))ExitProcess(ERROR_WRITE_FAULT);
    active=false;
    domain::session::DebugLog("[StartupSounds] battleReady=1 early=%u deferred=%u elapsedUs=%lld",
        early,200-early,diagnostics::startup::QpcUs()-begin);
}
}
