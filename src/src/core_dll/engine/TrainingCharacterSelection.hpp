#pragma once
#include <array>
#include <cstdint>

namespace cccaster::training_character {
// 通常31体と、キャラ定義・戦闘資産のある隠し／ボス8体。通信の選択番号は変更しない。
inline constexpr std::array<uint32_t, 39> Characters{
    22,7,51,15,28,8,2,0,30,11,9,31,4,3,1,19,12,13,14,29,17,18,33,23,10,25,35,5,20,6,34,
    16,32,53,58,59,72,73,85};
inline constexpr unsigned Columns = 10, MoonCount = 4;
constexpr uint32_t MoonValue(uint32_t character, unsigned slot) {
    return slot < 3 ? slot : character == 53 ? 8 : 9;
}
constexpr unsigned MoonSlot(uint32_t moon) { return moon < 3 ? moon : 3; }
constexpr uint32_t CursorCharacter(uint32_t character) {
    switch (character) {
    case 16: case 53: return 3;
    case 32: return 0;
    case 58: return 8;
    case 59: return 9;
    case 72: return 22;
    case 73: return 23;
    case 85: return 35;
    default: return character;
    }
}
struct Choice { uint32_t character = 0, moon = 0; };
enum class Field { Player, Character, Moon, Confirm };
enum class Action { None, Up, Down, Left, Right, Accept, Cancel };
enum class Result { None, Cancelled, Apply };
struct Selection {
    std::array<Choice, 2> original{};
    Choice choice{};
    unsigned player = 0, index = 0;
    Field field = Field::Character;
    bool open = false;
    std::array<uint8_t, Characters.size()> available = [] {
        std::array<uint8_t, Characters.size()> values{}; values.fill(15); return values;
    }();
    bool MoonAvailable(unsigned slot) const { return slot < MoonCount && (available[index] & (1u << slot)); }
    void NormalizeMoon() {
        unsigned slot = MoonSlot(choice.moon);
        if (!MoonAvailable(slot))
            for (unsigned i = 0; i < MoonCount; ++i) if (MoonAvailable(i)) { slot = i; break; }
        choice.moon = MoonValue(choice.character,slot);
    }
    void SetPlayer(unsigned side) {
        player = side % 2;
        choice = original[player];
        index = 0;
        for (unsigned i = 0; i < Characters.size(); ++i)
            if (Characters[i] == choice.character) index = i;
        choice.character = Characters[index];
        NormalizeMoon();
    }
    void Open(std::array<Choice, 2> current, unsigned side) {
        original = current; SetPlayer(side); field = Field::Character; open = true;
    }
    Result Step(Action action) {
        if (!open || action == Action::None) return Result::None;
        if (action == Action::Cancel) {
            if (field == Field::Player || field == Field::Character) {
                open = false; return Result::Cancelled;
            }
            field = field == Field::Confirm ? Field::Moon : Field::Character;
            return Result::None;
        }
        if (action == Action::Accept) {
            if (field == Field::Confirm) { open = false; return Result::Apply; }
            field = static_cast<Field>(static_cast<int>(field) + 1);
            return Result::None;
        }
        const int horizontal = action == Action::Left ? -1 : action == Action::Right ? 1 : 0;
        switch (field) {
        case Field::Player:
            if (horizontal) SetPlayer(1 - player);
            if (action == Action::Down) field = Field::Character;
            break;
        case Field::Character:
            if (horizontal) index = (index + Characters.size() + horizontal) % Characters.size();
            if (action == Action::Up) {
                if (index < Columns) field = Field::Player; else index -= Columns;
            }
            if (action == Action::Down) {
                if (index + Columns >= Characters.size()) field = Field::Moon; else index += Columns;
            }
            choice.character = Characters[index];
            NormalizeMoon();
            break;
        case Field::Moon:
            if (horizontal) {
                auto slot = MoonSlot(choice.moon);
                for (unsigned i = 0; i < MoonCount; ++i) {
                    slot = (slot + MoonCount + horizontal) % MoonCount;
                    if (MoonAvailable(slot)) { choice.moon = MoonValue(choice.character,slot); break; }
                }
            }
            if (action == Action::Up) field = Field::Character;
            if (action == Action::Down) field = Field::Confirm;
            break;
        case Field::Confirm:
            if (action == Action::Up) field = Field::Moon;
            break;
        }
        return Result::None;
    }
};
}
