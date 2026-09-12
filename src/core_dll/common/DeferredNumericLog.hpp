#pragma once
#include <array>
#include <cstdint>
#include <type_traits>
#include <utility>
#include "core_dll/common/DebugLog.hpp"
namespace cccaster::diagnostics {
// 計測区間専用。書式はリテラル、整数引数だけコピー。初回TLS確保はPrepareで前倒し。
// 整形・ログ共有ロックは後へ移すが、MinGW内部のTLS検索コストは残る。
struct DeferredNumericLog {
    struct Entry {
        const char *format;
        std::array<int64_t, 24> values;
        void (*emit)(const Entry &);
    };
    inline static thread_local bool active = false;
    inline static thread_local std::array<Entry, 32> entries{};
    inline static thread_local unsigned count = 0, dropped = 0;
    static void Prepare() {
        // MinGW emutlsは初回参照で確保する。締切前に全バッファを実体化する。
        entries[0].format = nullptr;
        count = dropped = 0;
        active = true;
    }
    template<class... Args, size_t... I>
    static void Emit(const Entry &e, std::index_sequence<I...>) {
        domain::session::DebugLog(e.format, static_cast<Args>(e.values[I])...);
    }
    template<class... Args>
    static void Log(const char *format, Args... args) {
        static_assert((std::is_integral_v<Args> && ...));
        static_assert(sizeof...(Args) <= 24);
        if (!active) { domain::session::DebugLog(format, args...); return; }
        if (count == entries.size()) { ++dropped; return; }
        auto &entry = entries[count++];
        entry.format = format;
        entry.values = {static_cast<int64_t>(args)...};
        entry.emit = [](const Entry &e) { Emit<Args...>(e, std::index_sequence_for<Args...>{}); };
    }
    static void Flush() {
        active = false;
        for (unsigned i = 0; i < count; ++i) entries[i].emit(entries[i]);
        if (dropped) domain::session::DebugLog("[DeferredTraceDropped] count=%u", dropped);
        count = dropped = 0;
    }
};
} // namespace cccaster::diagnostics
