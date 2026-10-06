#pragma once
#include "MatchingController.hpp"
#include "GuiProtocol.hpp"
#include "EmblemImage.hpp"
#include "ControllerSettings.hpp"
#include <future>

namespace cccaster::gui {
// RmlUiは操作要求と表示だけを担当する。通信・起動の状態はネイティブ側が所有する。
class LauncherModel {
    Session session_;
    MatchingController matching_;
    emblem::Image emblem_;
    ControllerSettings controllers_;
    std::string error_, emblemData_;
    std::future<bool> codeLookup_;
    Json pendingConnection_;
    bool profileError_ = false;
    void SaveString(const char* section, const char* key, const std::string& value);
    void SaveInt(const char* section, const char* key, int value);
    void UpdateEmblem();
    void ClearNotice();
    bool Occupied() const;
public:
    LauncherModel();
    void Poll();
    void PollController() { controllers_.Poll(Occupied()); }
    bool ControllerKey(UINT message, WPARAM key, LPARAM flags) {
        if(Occupied()) { controllers_.Poll(true);return false; }
        return controllers_.KeyMessage(message,key,flags);
    }
    void Command(const Json& command);
    Json State(bool includeLog) const;
    bool GameRunning() const { return session_.Running() && session_.booting; }
    bool TrainingStandbyRunning() const { return matching_.TrainingRunning(session_); }
};
}
