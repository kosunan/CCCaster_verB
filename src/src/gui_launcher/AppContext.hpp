#pragma once
#include <windows.h>
#include <filesystem>
#include <string>

namespace cccaster::gui {
inline std::filesystem::path exePath;
inline HWND guiWindow = nullptr;
inline bool japanese = true;
inline const char* Text(const char* english, const char* translated) {
    return japanese ? translated : english;
}
struct Message {
    std::string english, translated;
    const char* c_str() const { return japanese ? translated.c_str() : english.c_str(); }
};
int RunWorker(int argc, wchar_t** argv);
}
