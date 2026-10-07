#pragma once
#include <array>
namespace cccaster::training_palette {
// 6行×7ページ。37〜42は表示用で、ゲーム本来の色番号へは渡さない。
constexpr unsigned ColorRows=6, ColorPages=7, MenuColors=42;
constexpr unsigned ColorPageMove(unsigned color,bool next) {
    return (color+(next ? ColorRows : MenuColors-ColorRows))%MenuColors;
}
constexpr unsigned ColorRank(unsigned color,unsigned selected) {
    return ((color/ColorRows+ColorPages-selected/ColorRows)%ColorPages)*ColorRows+color%ColorRows;
}
}
