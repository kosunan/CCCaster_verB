#pragma once
#include "GuiProtocol.hpp"
#include <windows.h>
#include <memory>

namespace cccaster::gui {
// ゲームと同じ機器GUID・INI形式を使う、ランチャー専用の入力設定。
class ControllerSettings {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    ControllerSettings();
    ~ControllerSettings();
    void Command(const Json& command, bool locked);
    void Poll(bool locked);
    bool KeyMessage(UINT message, WPARAM key, LPARAM flags);
    Json State(bool locked) const;
};
}
