#pragma once
#include <array>
#include <string>

namespace cccaster::input {

enum class BindingKind { None, Key, Button, Axis, Hat };
struct CompiledBinding {
    BindingKind kind = BindingKind::None;
    int index = 0;
    int direction = 0;
};

// 設定の公開時だけ解析する。旧設定で受理していたstoiの数値接頭辞も維持する。
// 機器状態・押下履歴は保持せず、採取するたびに現在の状態で評価する。
template <class ResolveKey>
CompiledBinding CompileBinding(const std::string &text, bool keyboard, ResolveKey resolveKey) {
    if (text.empty()) return {};
    if (keyboard) {
        const int key = resolveKey(text);
        return key ? CompiledBinding{BindingKind::Key, key} : CompiledBinding{};
    }
    try {
        if (text[0] == 'B') {
            const int index = std::stoi(text.substr(1));
            if (index >= 0 && index < 128) return {BindingKind::Button, index};
        } else if (text[0] == 'A') {
            const int index = std::stoi(text.substr(1, text.length() - 2));
            if (index >= 0 && index < 8)
                return {BindingKind::Axis, index, text.back() == '+' ? 1 : -1};
        } else if (text[0] == 'H') {
            const auto separator = text.find('_');
            if (separator != std::string::npos) {
                const int index = std::stoi(text.substr(1, separator - 1));
                const int direction = std::stoi(text.substr(separator + 1));
                if (index >= 0 && index < 4 &&
                    (direction == 8 || direction == 2 || direction == 4 || direction == 6))
                    return {BindingKind::Hat, index, direction};
            }
        }
    } catch (...) {
        // 不正・範囲外の設定は従来どおり無入力。
    }
    return {};
}

struct CompiledInputBindings {
    enum Slot { A, B, C, D, E, Start, FN1, FN2, AB, Up, UpAlt, Down, DownAlt,
                Left, LeftAlt, Right, RightAlt, Count };
    inline static constexpr const char *Keys[] = {
        "A", "B", "C", "D", "E", "Start", "FN1", "FN2", "A+B",
        "Up", "Up_Alt", "Down", "Down_Alt", "Left", "Left_Alt", "Right", "Right_Alt"};
    inline static constexpr const char *Defaults[] = {
        "B0", "B1", "B2", "B3", "B4", "B7", "B8", "B9", "",
        "H0_8", "A1+", "H0_2", "A1-", "H0_4", "A0-", "H0_6", "A0+"};
    std::array<CompiledBinding, Count> bindings{};

    template <class ReadConfig, class ResolveKey>
    void Reload(bool keyboard, ReadConfig read, ResolveKey resolveKey) {
        for (int i = 0; i < Count; ++i)
            bindings[i] = CompileBinding(read(Keys[i], Defaults[i]), keyboard, resolveKey);
    }
    const CompiledBinding &operator[](Slot slot) const { return bindings[slot]; }
};
} // namespace cccaster::input
