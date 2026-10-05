#pragma once
#include "core_dll/engine/TrainingCharacterSelection.hpp"
#include <string>
#include <vector>

namespace cccaster::training_character {
// ゲームスレッドでのみ操作・参照。描画も同じスレッドのEndSceneから行う。
const Selection& Current();
bool Busy();
const char* Error();
void ObserveMenu(uint32_t* menu, uint32_t* command);
int PortraitIndex(uint32_t character);
int MoonPortraitIndex(uint32_t moon);
std::string CharacterName(uint32_t character);
bool ReadImage(const char* path, std::vector<uint8_t>& bytes);
}
