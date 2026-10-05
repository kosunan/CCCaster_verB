#pragma once
#include <filesystem>
#include <string_view>
namespace cccaster {
// 内部の文字列パスはUTF-8。Win32/ファイルストリームへはnative pathで渡す。
inline std::filesystem::path Utf8Path(std::string_view value) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(value.data()), value.size()));
}
inline std::string PathUtf8(const std::filesystem::path &path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}
}
