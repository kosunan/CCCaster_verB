#include "core_dll/common/DataPaths.hpp"

#include <mutex>

namespace cccaster::core::paths {

namespace {
std::mutex g_mutex;
std::string g_root; // 末尾に区切り文字を含む。空なら未設定。
} // namespace

void SetDataRoot(const std::string &absoluteDir) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (absoluteDir.empty())
        return;

    g_root = absoluteDir;
    const char last = g_root.back();
    if (last != '\\' && last != '/')
        g_root += '\\';
}

std::string Resolve(const std::string &fileName) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_root.empty())
        return fileName; // harness / 単体テスト
    return g_root + fileName;
}

std::string GetDataRoot() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_root;
}

} // namespace cccaster::core::paths
