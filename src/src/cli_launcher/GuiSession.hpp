#pragma once
#include <windows.h>

namespace cccaster::main_app::gui {
// GUI worker だけが設定する。通常CLI・自動試験の操作方法は変えない。
inline HANDLE cancelEvent = nullptr;
// GUIワーカー起動引数だけで設定する。CLIはINIの優先設定を参照しない。
inline int hostPreference = 0;
inline bool IsWorker() { return cancelEvent != nullptr; }
inline bool Cancelled() {
    return cancelEvent && WaitForSingleObject(cancelEvent, 0) == WAIT_OBJECT_0;
}
}
