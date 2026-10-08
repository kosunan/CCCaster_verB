#pragma once
#include "cli_launcher/ConfigManager.hpp"
#include <array>
#include <charconv>
#include <string>

namespace cccaster::domain::scene::selection_preferences {
// ディレイは起動・同期設定から決めるため、この保存形式には含めない。
enum class Key : unsigned { Animation, Hud, Width, Height, Fullscreen, CharacterFilter, ScreenFilter, AspectRatio, ViewFps, Count };
inline constexpr std::array<const char*, unsigned(Key::Count)> Names{
    "StageAnimation", "HudMode", "RenderWidth", "RenderHeight", "Fullscreen",
    "CharacterFilter", "ScreenFilter", "AspectRatio", "ViewFps"};
inline bool Valid(Key key, int value) {
    switch (key) {
    case Key::Width: return value >= 640 && value <= 16384;
    case Key::Height: return value >= 480 && value <= 16384;
    case Key::Hud: return value >= 0 && value < 3;
    case Key::CharacterFilter: return value >= 0 && value < 4;
    case Key::AspectRatio: return value >= 0 && value < 7;
    case Key::Animation: case Key::Fullscreen: case Key::ScreenFilter: case Key::ViewFps:
        return value == 0 || value == 1;
    default: return false;
    }
}
class Store {
    std::string path_;
    std::array<int, Names.size()> values_;
    bool Write(const std::array<int, Names.size()>& next) {
        if (path_.empty()) return false;
        main_app::Config config;
        config.Load(path_);
        for (unsigned i = 0; i < next.size(); ++i)
            if (Valid(Key(i), next[i])) config.SetInt("Display", Names[i], next[i]);
        if (!config.SaveChecked(path_)) return false;
        values_ = next;
        return true;
    }
public:
    Store() { values_.fill(-1); }
    void Load(const std::string& path) {
        path_ = path; values_.fill(-1);
        if (path.empty()) return;
        main_app::Config config; config.Load(path);
        for (unsigned i = 0; i < values_.size(); ++i) {
            const auto text = config.GetString("Display", Names[i]);
            int value = -1;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && Valid(Key(i), value)) values_[i] = value;
        }
        if (Get(Key::Width) < 0 || Get(Key::Height) < 0) values_[unsigned(Key::Width)] = values_[unsigned(Key::Height)] = -1;
    }
    int Get(Key key) const { return unsigned(key) < values_.size() ? values_[unsigned(key)] : -1; }
    bool Set(Key key, int value) {
        if (!Valid(key, value) || key == Key::Width || key == Key::Height) return false;
        auto next = values_; next[unsigned(key)] = value;
        return Write(next);
    }
    bool SetResolution(int width, int height) {
        if (!Valid(Key::Width, width) || !Valid(Key::Height, height)) return false;
        auto next = values_; next[unsigned(Key::Width)] = width; next[unsigned(Key::Height)] = height;
        return Write(next);
    }
};
void Initialize();
// 初回のキャラ選択で復元。解像度の非同期Reset中だけ入力を待たせる。
bool Restore();
void Save(Key key, int value);
void SaveResolution(int width, int height);
bool SaveFailed();
}
