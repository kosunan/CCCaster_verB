#include "StartupFontCache.hpp"
#include "FontAtlasSnapshot.hpp"
#include "core_dll/common/DataPaths.hpp"
#include "core_dll/common/StartupTrace.hpp"
#include "shared_contracts/NativePath.hpp"
#include <cstdlib>

namespace cccaster::hud::startup_fonts {
namespace {
std::filesystem::path Directory() {
    // 子プロセスの環境はUTF-16。getenvのANSI変換をUTF-8として解釈しない。
    constexpr auto name = L"CCCASTER_STARTUP_RESTORE_DIR";
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size) {
        std::wstring value(size, L'\0');
        const auto read = GetEnvironmentVariableW(name, value.data(), size);
        if (read && read < size) { value.resize(read); return std::filesystem::path(value) / "fonts"; }
    }
    return Utf8Path(core::paths::Resolve("startup_restore_v1")) / "fonts";
}
bool Build(ImFontAtlas* atlas) {
    using namespace font_snapshot;
    Digest key{}; Bytes saved;
    struct Input { ImWchar fallback, ellipsis; };
    std::vector<Input> inputs;
    bool eligible = false, hit = false;
    // 既存のatlasオブジェクト・ImFontポインターは保持。GPU textureは通常backendで再作成する。
    try {
        Store store(Directory(), 2);
        eligible = Key(atlas, key);
        for (auto* font : atlas->Fonts) inputs.push_back({font->FallbackChar, font->EllipsisChar});
        hit = eligible && store.Read(key, saved) && Decode(atlas, saved);
        if (hit && !std::getenv("CCCASTER_STARTUP_RESTORE_VERIFY")) {
            domain::session::DebugLog("[StartupFontCache] hit=1 fonts=%d bytes=%u", atlas->Fonts.Size, unsigned(saved.size()));
            return true;
        }
    } catch (...) { eligible = hit = false; }
    if (inputs.size() == size_t(atlas->Fonts.Size)) for (int i = 0; i < atlas->Fonts.Size; ++i) {
        atlas->Fonts[i]->FallbackChar = inputs[i].fallback;
        atlas->Fonts[i]->EllipsisChar = inputs[i].ellipsis;
    }
    const bool ok = ImFontAtlasGetBuilderForStbTruetype()->FontBuilder_Build(atlas);
    if (ok && eligible) try {
        const auto current = Encode(atlas);
        const bool equal = hit && current == saved;
        bool wrote = false;
        if (!current.empty() && !equal) {
            Store store(Directory(), 2);
            wrote = store.Write(key, current);
        }
        domain::session::DebugLog("[StartupFontCache] hit=%u saved=%u verified=%u different=%u fonts=%d bytes=%u",
            unsigned(hit), unsigned(wrote), unsigned(equal), unsigned(hit && !equal), atlas->Fonts.Size, unsigned(current.size()));
    } catch (...) { /* 生成済みの通常atlasをそのまま使用する。 */ }
    return ok;
}
const ImFontBuilderIO builder{Build};
}
void Install(ImFontAtlas* atlas) {
    if (!atlas->FontBuilderIO && !diagnostics::startup::Baseline() &&
        !std::getenv("CCCASTER_STARTUP_RESTORE_BASELINE")) atlas->FontBuilderIO = &builder;
}
}
