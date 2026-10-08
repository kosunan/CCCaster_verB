#pragma once
#include "core_dll/engine/TrainingPalette.hpp"
#include "core_dll/engine/ExtraColorFile.hpp"
#include "core_dll/mbaa_mem/GameInput.hpp"

namespace cccaster::training_palette {
bool Install();
inline bool editorOpen = false;
inline bool escapeHeld = false;
inline bool Active() { return editorOpen; }
unsigned Session();
unsigned InitialSlot();
void Open(unsigned slot);
void Close();
void Step(game_interface::GameInput& p1, game_interface::GameInput& p2, bool configuring);
const Asset* Get(unsigned slot);
const char* Error();
// 編集中は下書きだけ保持し、Closeで両側をまとめてゲームへ反映する。
const ExtraColor* Pending(unsigned slot);
void StagePackage(unsigned slot,const ExtraColor& value);
bool Apply(unsigned slot, const Edit& edit,unsigned baseColor=UINT32_MAX);
bool ApplyPackage(unsigned slot,const ExtraColor& value,bool close=true);
// 選択画面の通常立ち画像から、色番号ごとの描画タイル位置を採取する。
std::array<std::vector<uint32_t>,256> SampleBody(uint32_t* cg);
}
