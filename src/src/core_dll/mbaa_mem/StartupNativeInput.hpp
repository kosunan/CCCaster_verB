#pragma once
#include "core_dll/mbaa_mem/StartupSystemInfo.hpp"
namespace cccaster::game_memory::startup_native_input {
inline bool active = false;
inline std::array<uint8_t,5> initializeCall{};
__attribute__((naked)) inline void InitializeEmptyDevices() {
    __asm__ __volatile__(
        "movl %eax,0x5542E4\n\t" // 40FF50のHWND保存は維持。
        "movl $0x40FFB0,%eax\n\tcall *%eax\n\tret\n\t");
}
inline uint64_t Hash(uintptr_t address,size_t size) {
    uint64_t hash=14695981039346656037ull;
    for(size_t i=0;i<size;++i)hash=(hash^reinterpret_cast<const uint8_t*>(address)[i])*1099511628211ull;
    return hash;
}
inline bool Change(bool enable) {
    // DirectInputオブジェクトの参照より前で分岐。生成しない場合はnullのまま。
    constexpr std::array<uint8_t,5> original{0xA1,0xE8,0x44,0x55,0}, skip{0xE9,0x16,0,0,0};
    constexpr uint8_t tail[]{0x8B,0x08,0x8B,0x51,0x10,0x6A,0x01,
        0x68,0xB4,0xE9,0x76,0,0x68,0x90,0x04,0x41,0,0x6A,4,0x50,0xFF,0xD2,
        0xB8,1,0,0,0,0x5E,0xC3};
    if (std::memcmp(reinterpret_cast<void*>(0x40FFDA),tail,sizeof(tail))) return false;
    constexpr std::array<uint8_t,5> nativeCall{0xE8,0x12,0xFF,0xF6,0xFF};
    const patch::Spec specs[]{
        {"startup_native_joystick_enum",0x40FFD5,enable ? original : skip,enable ? skip : original},
        {"startup_native_devices",0x4A0039,enable ? nativeCall : initializeCall,enable ? initializeCall : nativeCall}};
    const auto result=patch::Apply(specs);
    if(result.rollbackFailed) ExitProcess(ERROR_WRITE_FAULT);
    if(result)active=enable;
    return bool(result);
}
inline void Initialize() {
    // 元ゲームの入力上書きは既に無効。CCCaster側が列挙・設定・入力を所有する。
    // ゲーム本体の未使用デバイス生成も省略。ポーリング/解放のnull経路を事前に照合する。
    if(!startup_system_info::dialogSkipped || std::getenv("CCCASTER_STARTUP_MINIMAL_BASELINE"))return;
    if(Hash(0x4A0030,38)!=0xe061b883d3e784f7ull ||
       Hash(0x40FFB0,71)!=0xba4dd1297a27a185ull ||
       Hash(0x410870,181)!=0x58f3ce31d518a708ull ||
       Hash(0x410930,331)!=0xa5f8c80ba1da37d0ull ||
       Hash(0x410A80,466)!=0xaaf87a6a1842680dull ||
       Hash(0x40FE80,207)!=0x21cc23ba646c0615ull) {
        domain::session::DebugLog("[StartupNativeInput] rejected=signature");return;
    }
    initializeCall={0xE8,0,0,0,0};
    const auto relative=uint32_t(uintptr_t(&InitializeEmptyDevices)-0x4A003E);
    std::memcpy(initializeCall.data()+1,&relative,4);
    domain::session::DebugLog("[StartupNativeInput] duplicateDevicesSkipped=%u",unsigned(Change(true)));
}
inline void Restore() {
    if(active && !Change(false))ExitProcess(ERROR_WRITE_FAULT);
}
}
