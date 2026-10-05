#include "StartupFileRead.hpp"
#include "core_dll/mbaa_mem/StartupPatch.hpp"
#include "core_dll/common/StartupTrace.hpp"
#include "shared_contracts/ProcessMemory.hpp"
#include <atomic>
#include <cstdlib>

namespace {
bool active = false, verify = false, inlineRead = false;
std::atomic<unsigned> reads{0};
constexpr uintptr_t Site = 0x413F80, WorkerOperand = 0x413F88;
constexpr std::array<uint8_t, 6> Original{0x8D,0x4E,0x40,0x51,0x6A,0};
std::array<uint8_t, 6> replacement{};
constexpr uint32_t Worker = 0x414180;
uint32_t diagnosticWorker = 0;

uint64_t Hash(const void* data, size_t size) {
    uint64_t hash = 14695981039346656037ull;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}
bool Signature() {
    // Ghidraの同期入口と補完逆アセンブルのworker全体。IATの値ではなく命令を照合。
    return cccaster::game_memory::startup::MatchesMenuCode() &&
        Hash(reinterpret_cast<void*>(0x413E70), 272) == 0x125d3890c6de659eull &&
        Hash(reinterpret_cast<void*>(0x413F86), 37) == 0xe309dba47eb33c21ull &&
        Hash(reinterpret_cast<void*>(0x413FB0), 77) == 0x4ad81656d39ff560ull &&
        Hash(reinterpret_cast<void*>(Worker), 276) == 0x026c0d05a2847a8bull;
}
}

extern "C" {
__attribute__((force_align_arg_pointer)) DWORD __cdecl cc_startup_read(void* object) {
    // 元workerはCreateThreadに渡されるがRET 4ではなくRET。実命令に合わせcdeclで呼ぶ。
    const auto result = reinterpret_cast<DWORD (__cdecl*)(void*)>(Worker)(object);
    reads.fetch_add(1, std::memory_order_relaxed);
    if (verify) {
        const auto* fields = static_cast<const uint32_t*>(object);
        cccaster::domain::session::DebugLog(
            "[StartupReadData] offset=%u bytes=%u requested=%u result=%lu hash=%016llX",
            fields[4], fields[8], fields[9], static_cast<unsigned long>(result),
            static_cast<unsigned long long>(Hash(reinterpret_cast<void*>(fields[12]), fields[8])));
    }
    return result;
}
__attribute__((force_align_arg_pointer)) DWORD WINAPI cc_startup_read_thread(void* object) {
    return cc_startup_read(object);
}
__attribute__((naked)) void cc_startup_read_gate() {
    __asm__ __volatile__(
        // 413E70がEBP/ESIをpushした後。同期読込み413FB0からの呼出しだけを対象にする。
        "cmpl $0x413FC9,8(%esp)\n\t"
        "jne 1f\n\t"
        "cmpl $0,0x3c(%esi)\n\t"
        "jne 1f\n\t"
        "pushl %esi\n\t"
        "call _cc_startup_read\n\t"
        "addl $4,%esp\n\t"
        // threadは生成していない。元の終了待ち・解放はnullのまま通過する。
        "movl $0,0x3c(%esi)\n\t"
        "movl $0,0x40(%esi)\n\t"
        "movl $0x413FA1,%eax\n\t"
        "jmp *%eax\n\t"
        // 非同期入口の引数・CreateThread・戻り値判定は元のまま。
        "1: leal 0x40(%esi),%ecx\n\t"
        "pushl %ecx\n\t"
        "pushl $0\n\t"
        "movl $0x413F86,%eax\n\t"
        "jmp *%eax\n\t");
}
}

namespace cccaster::game_memory::startup_file_read {
bool Change(bool enable) {
    std::vector<patch::Spec> sites;
    if (inlineRead) sites.push_back({"startup_sync_read", Site,
        enable ? Original : replacement, enable ? replacement : Original});
    const auto original = std::span(reinterpret_cast<const uint8_t*>(&Worker), 4);
    const auto diagnostic = std::span(reinterpret_cast<const uint8_t*>(&diagnosticWorker), 4);
    if (verify) sites.push_back({"startup_read_verify", WorkerOperand,
        enable ? original : diagnostic, enable ? diagnostic : original});
    const auto result = patch::Apply(sites);
    if (!result) {
        domain::session::DebugLog("[StartupFileRead] FAILED site=%s error=%s rollbackFailed=%u",
            result.name, patch::Name(result.error), unsigned(result.rollbackFailed));
        if (result.rollbackFailed) ExitProcess(ERROR_WRITE_FAULT);
        return false;
    }
    active = enable;
    return true;
}
void Initialize(uint8_t mode) {
    if (mode > 1 || !diagnostics::startup::HasGate() || diagnostics::startup::Baseline()) return;
    inlineRead = std::getenv("CCCASTER_STARTUP_IO_BASELINE") == nullptr;
    verify = std::getenv("CCCASTER_STARTUP_IO_VERIFY") != nullptr;
    if (!inlineRead && !verify) return;
    if (!Signature()) {
        domain::session::DebugLog("[StartupFileRead] signature mismatch; native reader retained");
        return;
    }
    replacement = {0xE9,0,0,0,0,0x90};
    const auto relative = uint32_t(reinterpret_cast<uintptr_t>(&cc_startup_read_gate) - (Site + 5));
    std::memcpy(replacement.data() + 1, &relative, 4);
    diagnosticWorker = uint32_t(reinterpret_cast<uintptr_t>(&cc_startup_read_thread));
    domain::session::DebugLog("[StartupFileRead] enabled=%u inline=%u verify=%u",
        unsigned(Change(true)), unsigned(inlineRead), unsigned(verify));
}
void Restore() {
    if (!active) return;
    if (!Change(false)) ExitProcess(ERROR_WRITE_FAULT);
    domain::session::DebugLog("[StartupFileRead] restored=1 reads=%u inline=%u",
        reads.load(std::memory_order_relaxed), unsigned(inlineRead));
}
}
