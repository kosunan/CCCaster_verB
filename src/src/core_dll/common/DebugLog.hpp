#pragma once
// ============================================================================
// DebugLog — HookLog ベースのフォーマット付きログ出力
// 全Sceneおよび SceneRunner から共通で使用する
// ============================================================================

#include <cstdio>
#include <cstdarg>

extern void HookLog(const char *msg);

namespace cccaster::domain::session {

inline void DebugLog(const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    HookLog(buf);
}

} // namespace cccaster::domain::session
