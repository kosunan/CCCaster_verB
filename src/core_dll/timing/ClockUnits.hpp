#pragma once
#include <cstdint>
namespace cccaster::core::timer {
// 時刻は1/60µs。補正の割算で生じる1tick未満もPartsで持ち越す。
inline constexpr int64_t ClockSecond = 60000000;
inline constexpr int64_t ClockFrame = 1000000;
inline constexpr int64_t ClockParts = 1000000;
}

