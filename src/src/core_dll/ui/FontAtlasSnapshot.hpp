#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include "shared_contracts/StartupCache.hpp"
#include <cmath>

namespace cccaster::hud::font_snapshot {
using namespace startup_cache;
// この形式を変えたら必ずSchemaを更新する。アドレス・GPU識別子は含めない。
constexpr uint32_t Schema = 1;
struct Atlas {
    uint32_t version = Schema, width = 0, height = 0, fonts = 0, rects = 0;
    int32_t mouse = -1, lines = -1;
    ImVec2 white{};
    ImVec4 lineUvs[IM_DRAWLIST_TEX_LINES_WIDTH_MAX + 1]{};
};
struct Font {
    float size = 0, ascent = 0, descent = 0, scale = 0, ellipsisWidth = 0, ellipsisStep = 0;
    uint32_t surface = 0, fallback = 0, ellipsis = 0, ellipsisCount = 0, glyphs = 0;
};
struct Glyph {
    uint32_t codepoint = 0, visible = 0, colored = 0;
    float advance = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0, u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};
struct Rect { uint32_t width = 0, height = 0, x = 0, y = 0; };
template<class T> void Put(Bytes& bytes, const T& value) {
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(value));
}
struct Reader {
    std::span<const uint8_t> bytes;
    template<class T> bool Get(T& value) {
        if (bytes.size() < sizeof(value)) return false;
        std::memcpy(&value, bytes.data(), sizeof(value)); bytes = bytes.subspan(sizeof(value)); return true;
    }
};
inline bool Key(ImFontAtlas* atlas, Digest& key) {
    // 現行HUDの一入力=一書体。将来merge/custom glyphを使う場合は通常ビルダーへ戻す。
    ImFontAtlasBuildInit(atlas);
    if (atlas->Fonts.Size < 1 || atlas->Fonts.Size > 64 || atlas->ConfigData.Size != atlas->Fonts.Size) return false;
    for (int i = 0; i < atlas->CustomRects.Size; ++i)
        if (i != atlas->PackIdMouseCursors && i != atlas->PackIdLines) return false;
    Hash hash;
    hash.Value(Schema); hash.Value(IMGUI_VERSION_NUM); hash.Value(sizeof(ImWchar));
    hash.Value(atlas->Flags); hash.Value(atlas->TexDesiredWidth); hash.Value(atlas->TexGlyphPadding);
    hash.Value(atlas->FontBuilderFlags); hash.Value(atlas->Fonts.Size);
    for (int i = 0; i < atlas->ConfigData.Size; ++i) {
        const auto& c = atlas->ConfigData[i];
        if (c.MergeMode || c.DstFont != atlas->Fonts[i] || !c.FontData || c.FontDataSize <= 0) return false;
        hash.Value(c.FontDataSize); hash.Add(c.FontData, c.FontDataSize);
        hash.Value(c.FontNo); hash.Value(c.SizePixels); hash.Value(c.OversampleH); hash.Value(c.OversampleV);
        hash.Value(c.PixelSnapH); hash.Value(c.GlyphExtraSpacing); hash.Value(c.GlyphOffset);
        hash.Value(c.GlyphMinAdvanceX); hash.Value(c.GlyphMaxAdvanceX); hash.Value(c.FontBuilderFlags);
        hash.Value(c.RasterizerMultiply); hash.Value(c.RasterizerDensity); hash.Value(c.EllipsisChar);
        hash.Value(atlas->Fonts[i]->Scale);
        hash.Value(atlas->Fonts[i]->FallbackChar); hash.Value(atlas->Fonts[i]->EllipsisChar);
        const ImWchar* ranges = c.GlyphRanges ? c.GlyphRanges : atlas->GetGlyphRangesDefault();
        unsigned count = 0;
        while (count < 65536 && ranges[count]) ++count;
        if (count == 65536) return false;
        hash.Value(count); hash.Add(ranges, (count + 1) * sizeof(ImWchar));
    }
    return hash.Finish(key);
}
inline Bytes Encode(const ImFontAtlas* atlas) {
    if (!atlas->TexReady || !atlas->TexPixelsAlpha8 || atlas->TexPixelsUseColors ||
        atlas->TexWidth < 1 || atlas->TexHeight < 1 || uint64_t(atlas->TexWidth) * atlas->TexHeight > Limit / 2) return {};
    Atlas meta{};
    meta.width = atlas->TexWidth; meta.height = atlas->TexHeight;
    meta.fonts = atlas->Fonts.Size; meta.rects = atlas->CustomRects.Size;
    meta.mouse = atlas->PackIdMouseCursors; meta.lines = atlas->PackIdLines;
    meta.white = atlas->TexUvWhitePixel;
    std::memcpy(meta.lineUvs, atlas->TexUvLines, sizeof(meta.lineUvs));
    Bytes bytes; Put(bytes, meta);
    for (const auto& r : atlas->CustomRects) Put(bytes, Rect{r.Width, r.Height, r.X, r.Y});
    for (const auto* f : atlas->Fonts) {
        Put(bytes, Font{f->FontSize, f->Ascent, f->Descent, f->Scale, f->EllipsisWidth, f->EllipsisCharStep,
            uint32_t(f->MetricsTotalSurface), f->FallbackChar, f->EllipsisChar, uint32_t(f->EllipsisCharCount), uint32_t(f->Glyphs.Size)});
        for (const auto& g : f->Glyphs)
            Put(bytes, Glyph{g.Codepoint, g.Visible, g.Colored, g.AdvanceX, g.X0, g.Y0, g.X1, g.Y1, g.U0, g.V0, g.U1, g.V1});
    }
    bytes.insert(bytes.end(), atlas->TexPixelsAlpha8, atlas->TexPixelsAlpha8 + meta.width * meta.height);
    return bytes;
}
inline bool Unit(float value) { return std::isfinite(value) && value >= 0 && value <= 1; }
inline bool Decode(ImFontAtlas* atlas, std::span<const uint8_t> bytes) {
    // 全件の寸法・範囲を検査してから実際のatlasへ反映する。
    Reader reader{bytes}; Atlas meta{};
    if (!reader.Get(meta) || meta.version != Schema || meta.fonts != unsigned(atlas->Fonts.Size) ||
        !meta.fonts || meta.fonts > 64 || meta.rects != unsigned(atlas->CustomRects.Size) || meta.rects > 2 ||
        meta.mouse != atlas->PackIdMouseCursors || meta.lines != atlas->PackIdLines ||
        !meta.width || !meta.height || meta.width > 16384 || meta.height > 16384 ||
        uint64_t(meta.width) * meta.height > Limit / 2 || !Unit(meta.white.x) || !Unit(meta.white.y)) return false;
    for (const auto& uv : meta.lineUvs) if (!Unit(uv.x) || !Unit(uv.y) || !Unit(uv.z) || !Unit(uv.w)) return false;
    std::vector<Rect> rects(meta.rects);
    for (unsigned i = 0; i < meta.rects; ++i) {
        auto& r = rects[i];
        if (!reader.Get(r) || r.width != atlas->CustomRects[i].Width || r.height != atlas->CustomRects[i].Height ||
            r.x > meta.width || r.y > meta.height || r.width > meta.width - r.x || r.height > meta.height - r.y) return false;
    }
    std::vector<Font> fonts(meta.fonts);
    std::vector<std::vector<Glyph>> glyphs(meta.fonts);
    for (unsigned i = 0; i < meta.fonts; ++i) {
        auto& f = fonts[i];
        if (!reader.Get(f) || !f.glyphs || f.glyphs >= 65534 || f.size != atlas->ConfigData[i].SizePixels ||
            !std::isfinite(f.ascent) || !std::isfinite(f.descent) || !std::isfinite(f.scale) || f.scale <= 0 ||
            !std::isfinite(f.ellipsisWidth) || !std::isfinite(f.ellipsisStep) || f.surface > INT32_MAX ||
            f.fallback > IM_UNICODE_CODEPOINT_MAX || f.ellipsis > IM_UNICODE_CODEPOINT_MAX || f.ellipsisCount > 3 ||
            f.glyphs > reader.bytes.size() / sizeof(Glyph)) return false;
        glyphs[i].resize(f.glyphs);
        for (auto& g : glyphs[i]) {
            if (!reader.Get(g) || g.codepoint > IM_UNICODE_CODEPOINT_MAX || g.visible > 1 || g.colored > 1 ||
                !std::isfinite(g.advance) || !std::isfinite(g.x0) || !std::isfinite(g.y0) ||
                !std::isfinite(g.x1) || !std::isfinite(g.y1) ||
                !Unit(g.u0) || !Unit(g.v0) || !Unit(g.u1) || !Unit(g.v1)) return false;
        }
    }
    const size_t pixels = size_t(meta.width) * meta.height;
    if (reader.bytes.size() != pixels) return false;
    auto* image = static_cast<unsigned char*>(IM_ALLOC(pixels));
    if (!image) return false;
    std::memcpy(image, reader.bytes.data(), pixels);
    atlas->ClearTexData();
    atlas->TexPixelsAlpha8 = image; atlas->TexPixelsUseColors = false;
    atlas->TexWidth = meta.width; atlas->TexHeight = meta.height;
    atlas->TexUvScale = ImVec2(1.0f / meta.width, 1.0f / meta.height); atlas->TexUvWhitePixel = meta.white;
    std::memcpy(atlas->TexUvLines, meta.lineUvs, sizeof(meta.lineUvs));
    for (unsigned i = 0; i < meta.rects; ++i) { atlas->CustomRects[i].X = rects[i].x; atlas->CustomRects[i].Y = rects[i].y; }
    for (unsigned i = 0; i < meta.fonts; ++i) {
        auto* font = atlas->Fonts[i]; const auto& f = fonts[i];
        font->ClearOutputData(); font->ContainerAtlas = atlas; font->ConfigData = &atlas->ConfigData[i]; font->ConfigDataCount = 1;
        font->FontSize = f.size; font->Ascent = f.ascent; font->Descent = f.descent;
        font->Scale = f.scale; font->MetricsTotalSurface = f.surface; font->FallbackChar = ImWchar(f.fallback);
        font->Glyphs.resize(f.glyphs);
        for (unsigned j = 0; j < f.glyphs; ++j) {
            const auto& g = glyphs[i][j]; auto& dst = font->Glyphs[j];
            dst.Codepoint = g.codepoint; dst.Visible = g.visible; dst.Colored = g.colored; dst.AdvanceX = g.advance;
            dst.X0 = g.x0; dst.Y0 = g.y0; dst.X1 = g.x1; dst.Y1 = g.y1;
            dst.U0 = g.u0; dst.V0 = g.v0; dst.U1 = g.u1; dst.V1 = g.v1;
        }
        font->BuildLookupTable();
        font->EllipsisChar = ImWchar(f.ellipsis); font->EllipsisCharCount = f.ellipsisCount;
        font->EllipsisWidth = f.ellipsisWidth; font->EllipsisCharStep = f.ellipsisStep;
    }
    atlas->TexReady = true;
    return true;
}
}
