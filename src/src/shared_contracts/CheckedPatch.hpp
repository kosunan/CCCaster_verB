#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace cccaster::patch {
// 呼出側が対象コードの停止を保証する。実行中コードの原子的な置換ではない。
enum class Error : uint32_t { None, InvalidSpec, Read, Mismatch, Protect, Write, Verify, Restore, Flush };
struct Result {
    Error error = Error::None;
    uint32_t systemError = 0;
    uintptr_t address = 0;
    const char *name = "";
    bool rollbackFailed = false;
    explicit operator bool() const { return error == Error::None; }
};
struct Spec {
    const char *name;
    uintptr_t address;
    std::span<const uint8_t> expected, replacement;
    bool executable = true;
};
inline const char *Name(Error e) {
    constexpr const char *names[] = {"none", "invalid_spec", "read", "signature", "protect",
        "write", "verify", "restore_protection", "flush_cache"};
    return uint32_t(e) < std::size(names) ? names[uint32_t(e)] : "unknown";
}
template<class Memory> Result Apply(Memory &memory, std::span<const Spec> specs) {
    struct Saved { std::array<uint8_t, 64> bytes{}; uint32_t protection = 0; bool touched = false; };
    std::vector<Saved> saved(specs.size());
    auto failure = [&](Error error, size_t i) {
        return Result{error, memory.LastError(), specs[i].address, specs[i].name, false};
    };
    // 全署名を事前照合。不一致があれば一切書き込まない。
    for (size_t i = 0; i < specs.size(); ++i) {
        const auto &s = specs[i];
        if (!s.address || s.expected.empty() || s.expected.size() > 64 || s.replacement.empty() ||
            s.replacement.size() > s.expected.size() || s.address + s.expected.size() < s.address ||
            !memory.SinglePage(s.address, s.expected.size()))
            return {Error::InvalidSpec, 0, s.address, s.name};
        for (size_t j = 0; j < i; ++j)
            if (s.address < specs[j].address + specs[j].expected.size() && specs[j].address < s.address + s.expected.size())
                return {Error::InvalidSpec, 0, s.address, s.name};
        if (!memory.Read(s.address, saved[i].bytes.data(), s.expected.size())) return failure(Error::Read, i);
        if (std::memcmp(saved[i].bytes.data(), s.expected.data(), s.expected.size()))
            return {Error::Mismatch, 0, s.address, s.name};
    }
    Result result;
    for (size_t i = 0; i < specs.size(); ++i) {
        const auto &s = specs[i];
        auto &old = saved[i];
        if (!memory.MakeWritable(s.address, s.replacement.size(), s.executable, old.protection)) {
            result = failure(Error::Protect, i); break;
        }
        old.touched = true; // Write失敗でも一部変更され得る。
        std::array<uint8_t, 64> readback{};
        if (!memory.Write(s.address, s.replacement.data(), s.replacement.size())) result = failure(Error::Write, i);
        else if (!memory.Read(s.address, readback.data(), s.replacement.size()) ||
                 std::memcmp(readback.data(), s.replacement.data(), s.replacement.size())) result = failure(Error::Verify, i);
        // 短絡評価で保護復元やキャッシュ更新を飛ばさない。
        if (!memory.RestoreProtection(s.address, s.replacement.size(), old.protection) && result) result = failure(Error::Restore, i);
        if (s.executable && !memory.Flush(s.address, s.replacement.size()) && result) result = failure(Error::Flush, i);
        if (!result) break;
    }
    if (result) return result;
    // 逆順に元バイトと保護を復元。復元失敗も明示し、ゲームを継続させない。
    for (size_t i = specs.size(); i-- > 0;) {
        if (!saved[i].touched) continue;
        const auto &s = specs[i];
        uint32_t ignored = 0;
        bool ok = memory.MakeWritable(s.address, s.replacement.size(), s.executable, ignored);
        if (ok) {
            const bool written = memory.Write(s.address, saved[i].bytes.data(), s.replacement.size());
            std::array<uint8_t, 64> restored{};
            const bool read = memory.Read(s.address, restored.data(), s.replacement.size());
            ok = written && read && !std::memcmp(restored.data(), saved[i].bytes.data(), s.replacement.size());
        }
        const bool protectedAgain = memory.RestoreProtection(s.address, s.replacement.size(), saved[i].protection);
        const bool flushed = !s.executable || memory.Flush(s.address, s.replacement.size());
        if (!ok || !protectedAgain || !flushed) result.rollbackFailed = true;
    }
    return result;
}
}
