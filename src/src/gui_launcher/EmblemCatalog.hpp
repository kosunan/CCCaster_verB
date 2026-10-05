#pragma once
// assets/emblems/generate.py から生成。
#include <string_view>
namespace cccaster::emblem {
struct Preset { const char* code; const char* japanese; const char* english; };
inline constexpr Preset Presets[]{
    {"jp", "日本", "Japan"},
    {"us", "アメリカ", "United States"},
    {"gb", "イギリス", "United Kingdom"},
    {"ca", "カナダ", "Canada"},
    {"br", "ブラジル", "Brazil"},
    {"ar", "アルゼンチン", "Argentina"},
    {"fr", "フランス", "France"},
    {"de", "ドイツ", "Germany"},
    {"it", "イタリア", "Italy"},
    {"es", "スペイン", "Spain"},
    {"pt", "ポルトガル", "Portugal"},
    {"nl", "オランダ", "Netherlands"},
    {"be", "ベルギー", "Belgium"},
    {"ie", "アイルランド", "Ireland"},
    {"se", "スウェーデン", "Sweden"},
    {"no", "ノルウェー", "Norway"},
    {"dk", "デンマーク", "Denmark"},
    {"fi", "フィンランド", "Finland"},
    {"ch", "スイス", "Switzerland"},
    {"pl", "ポーランド", "Poland"},
    {"ua", "ウクライナ", "Ukraine"},
    {"at", "オーストリア", "Austria"},
    {"cn", "中国", "China"},
    {"kr", "韓国", "South Korea"},
};
inline const Preset* FindPreset(std::string_view code) {
    for (const auto& preset : Presets) if (preset.code == code) return &preset;
    return nullptr;
}
}
