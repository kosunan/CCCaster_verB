#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include "core_dll/hook/ControllerProfile.hpp"

namespace cccaster::domain::ui {

inline std::string ControllerBindingName(const std::string& value) {
    if(value.empty()) return "--";
    if(value.size()>1 && value[0]=='B' && value.size()<5 &&
       value.find_first_not_of("0123456789",1)==std::string::npos)
        return "Button "+std::to_string(std::stoi(value.substr(1))+1);
    if(value.size()==4 && value[0]=='H' && value[2]=='_') {
        const char* dir=value[3]=='8' ? "UP" : value[3]=='2' ? "DOWN" :
                        value[3]=='4' ? "LEFT" : value[3]=='6' ? "RIGHT" : nullptr;
        if(dir) return "D-pad "+std::string(dir);
    }
    if(value=="LeftShift") return "L.Shift";
    if(value=="RightShift") return "R.Shift";
    if(value=="LeftCtrl") return "L.Ctrl";
    if(value=="RightCtrl") return "R.Ctrl";
    return value;
}

struct ControllerMappingValidation {
    bool valid = false;
    std::string message;
};

inline ControllerMappingValidation ValidateControllerMapping(const cccaster::input::Bindings &binds) {
    const auto &kNames = cccaster::input::BindingLabels;

    // 移動・基本攻撃・決定取消・メニュー開始は無割当のまま保存させない。
    for (int i = 0; i <= 9; ++i) {
        if (binds[i].empty())
            return {false, std::string(kNames[i]) + " is not assigned"};
    }

    std::unordered_map<std::string, int> firstUse;
    // トレーニング保存・読込は通常入力とも相互にも重複可。同時押しは読込優先。
    for (int i = 0; i < cccaster::input::SaveStateBinding; ++i) {
        if (binds[i].empty())
            continue; // FN1/FN2/A+Bは未使用を許可する。
        const auto [it, inserted] = firstUse.emplace(binds[i], i);
        if (!inserted)
            return {false, std::string(kNames[it->second]) + " and " + kNames[i] + " both use " + ControllerBindingName(binds[i])};
    }
    return {true, "Mapping is valid"};
}

} // namespace cccaster::domain::ui
