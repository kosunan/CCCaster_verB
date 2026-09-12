#pragma once
// ============================================================================
// test_support.hpp — 依存ゼロの最小テストハーネス
//
// 外部フレームワークを導入しないのは、mingw32 (32bit) 環境で依存を増やさずに
// ctest から実行できることを優先したため。test_overlay と同じく、
// 対象が必要とする外部シンボルは stub_*.cpp で置き換える。
//
// 使い方:
//   CC_CASE("名前");            // ケース開始
//   CC_CHECK(cond);             // 条件
//   CC_CHECK_EQ(actual, exp);   // 値の一致（失敗時に両方を表示）
//   return cccaster::test::Summarize("suite名");  // main の戻り値に使う
// ============================================================================

#include <cstdio>
#include <cstdint>

namespace cccaster::test {

inline int g_checks = 0;
inline int g_failures = 0;
inline const char *g_currentCase = "(no case)";

inline void BeginCase(const char *name) {
    g_currentCase = name;
    std::printf("[ RUN      ] %s\n", name);
}

inline void ReportFailure(const char *file, int line, const char *expr) {
    ++g_failures;
    std::printf("[  FAILED  ] %s\n            %s:%d\n            %s\n", g_currentCase, file, line, expr);
}

inline void ReportFailure(const char *file, int line, const char *expr, long long actual,
                          long long expected) {
    ++g_failures;
    std::printf("[  FAILED  ] %s\n            %s:%d\n            %s\n"
                "              actual   = %lld\n"
                "              expected = %lld\n",
                g_currentCase, file, line, expr, actual, expected);
}

inline int Summarize(const char *suite) {
    std::printf("--------------------------------------------\n"
                "%s: %d checks, %d failures\n",
                suite, g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}

} // namespace cccaster::test

#define CC_CASE(name) ::cccaster::test::BeginCase(name)

#define CC_CHECK(expr)                                                                                       \
    do {                                                                                                     \
        ++::cccaster::test::g_checks;                                                                        \
        if (!(expr)) {                                                                                       \
            ::cccaster::test::ReportFailure(__FILE__, __LINE__, #expr);                                      \
        }                                                                                                    \
    } while (0)

#define CC_CHECK_EQ(actual, expected)                                                                        \
    do {                                                                                                     \
        ++::cccaster::test::g_checks;                                                                        \
        const long long _cc_a = static_cast<long long>(actual);                                              \
        const long long _cc_e = static_cast<long long>(expected);                                            \
        if (_cc_a != _cc_e) {                                                                                \
            ::cccaster::test::ReportFailure(__FILE__, __LINE__, #actual " == " #expected, _cc_a, _cc_e);     \
        }                                                                                                    \
    } while (0)
