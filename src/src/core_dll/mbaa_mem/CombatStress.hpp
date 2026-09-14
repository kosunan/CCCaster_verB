#pragma once
#include <cstdint>
namespace cccaster::testing::combat_stress {
// 自動実機試験限定。1=近距離固定、2=さらに残りヒット数を補充。
bool Install();
bool Active();
void Begin(bool combat, uint32_t frame);
void Flush();
}
