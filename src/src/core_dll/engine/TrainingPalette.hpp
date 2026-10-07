#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

namespace cccaster::training_palette {
// 色はRGBAの順に下位バイトへ格納（ゲームのPALと同じ）。
constexpr uint32_t NoPixel = UINT32_MAX;
using Palette = std::array<uint32_t,256>;
struct Page {
    std::array<uint8_t, 65536> indices{};
    // valid: 1=色番号0だけ透明、2=別アルファ面、3=RGBA。
    std::array<uint8_t, 65536> valid{};
    std::vector<uint32_t> banks, direct;
    std::vector<uint8_t> alpha;
};
struct Tile { unsigned page, x, y, width, height; int left, top; };
struct Sprite {
    unsigned id = 0;
    std::string name;
    int left = 0, top = 0, width = 0, height = 0;
    std::vector<Tile> tiles;
    unsigned type = 0, bank = 0;
};
struct Edit {
    Palette palette{};
    std::map<uint32_t, Palette> effects;
    std::map<uint32_t, uint32_t> replacements;
    // ピクセルモードはパレットから独立したRGBを保持。タイルの共有は維持する。
    std::map<uint32_t, uint32_t> pixels;
    bool operator==(const Edit&) const = default;
    Palette* Bank(uint32_t id) {
        if (!id) return &palette;
        auto found=effects.find(id); return found==effects.end() ? nullptr : &found->second;
    }
    const Palette* Bank(uint32_t id) const { return const_cast<Edit*>(this)->Bank(id); }
};
struct Asset {
    uintptr_t owner = 0;
    std::map<unsigned, Page> pages;
    std::vector<Sprite> sprites;
    Edit original, applied;
    unsigned character = 0, component = 0, baseColor = 0;
    uint32_t layout = 0;
    std::string resource;
    std::vector<Palette> standard;
    bool Valid(uint32_t key) const {
        const auto page = pages.find(key / 65536);
        return page != pages.end() && page->second.valid[key % 65536];
    }
    int Index(uint32_t key) const {
        const auto page = pages.find(key / 65536);
        return Valid(key) && page->second.valid[key%65536]!=3 ? page->second.indices[key % 65536] : -1;
    }
    uint32_t Bank(uint32_t key) const {
        auto page=pages.find(key/65536);
        return page==pages.end() || page->second.banks.empty() ? 0 : page->second.banks[key%65536];
    }
    uint32_t Color(const Edit& edit, uint32_t key) const {
        if (!Valid(key)) return 0;
        const auto pixel = edit.pixels.find(key);
        if (pixel!=edit.pixels.end()) return pixel->second;
        const auto& page=pages.at(key/65536); const auto offset=key%65536;
        if (page.valid[offset]==3) {
            const auto color=page.direct[offset];
            const auto replacement=edit.replacements.find(color&0xffffff);
            return replacement==edit.replacements.end() ? color : (color&0xff000000)|(replacement->second&0xffffff);
        }
        const auto* colors=edit.Bank(Bank(key));
        if (!colors) return 0;
        const auto index=page.indices[offset];
        const auto alpha=page.valid[offset]==2 ? page.alpha[offset] : (index ? 255u : 0u);
        return ((*colors)[index]&0xffffff)|(alpha<<24);
    }
    uint32_t Locate(const Sprite& sprite, int x, int y) const {
        if (x < 0 || y < 0 || x >= sprite.width || y >= sprite.height) return NoPixel;
        x += sprite.left; y += sprite.top;
        // 描画と同じく後ろのタイルを優先する。
        for (auto tile = sprite.tiles.rbegin(); tile != sprite.tiles.rend(); ++tile) {
            const int dx = x - tile->left, dy = y - tile->top;
            if (dx < 0 || dy < 0 || dx >= int(tile->width) || dy >= int(tile->height)) continue;
            const auto key = tile->page * 65536 + (tile->y + dy) * 256 + tile->x + dx;
            if (Valid(key)) return key;
        }
        return NoPixel;
    }
};
class History {
    std::vector<Edit> undo, redo;
    Edit before;
    bool stroke = false;
public:
    void Begin(const Edit& value) { if (!stroke) { before = value; stroke = true; } }
    void End(const Edit& value) {
        if (stroke && !(before == value)) {
            if (undo.size() == 16) undo.erase(undo.begin());
            undo.push_back(std::move(before)); redo.clear();
        }
        stroke = false;
    }
    bool Undo(Edit& value) {
        End(value); if (undo.empty()) return false;
        redo.push_back(std::move(value)); value = std::move(undo.back()); undo.pop_back(); return true;
    }
    bool Redo(Edit& value) {
        End(value); if (redo.empty()) return false;
        undo.push_back(std::move(value)); value = std::move(redo.back()); redo.pop_back(); return true;
    }
};
inline unsigned Quantize(unsigned value, unsigned shift) {
    return value ? (std::max)(1u, value >> shift) : 0;
}
// 元の0x402EB0と同じ非ゼロ最小値を保つ16bit変換。
inline uint16_t Pack16(uint32_t rgba, bool argb4444) {
    const unsigned r = rgba & 255, g = (rgba >> 8) & 255, b = (rgba >> 16) & 255, a = rgba >> 24;
    return argb4444 ? uint16_t(Quantize(a,4)<<12 | Quantize(r,4)<<8 | Quantize(g,4)<<4 | Quantize(b,4))
                    : uint16_t((a ? 0x8000 : 0) | Quantize(r,3)<<10 | Quantize(g,3)<<5 | Quantize(b,3));
}
}
