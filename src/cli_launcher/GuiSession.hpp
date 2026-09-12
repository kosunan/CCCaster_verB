#pragma once
#include <windows.h>

namespace cccaster::main_app::gui {
// GUI worker だけが設定する。通常CLI・自動試験の操作方法は変えない。
inline HANDLE cancelEvent = nullptr;
inline bool IsWorker() { return cancelEvent != nullptr; }
inline bool Cancelled() {
    return cancelEvent && WaitForSingleObject(cancelEvent, 0) == WAIT_OBJECT_0;
}
}
