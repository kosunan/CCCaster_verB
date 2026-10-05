#pragma once
#include "GuiProtocol.hpp"
#include <windows.h>
#include <functional>
#include <memory>
#include <filesystem>

namespace cccaster::gui {
inline constexpr UINT ReloadDisplayMessage = WM_APP + 40;
inline constexpr UINT UiCommandMessage = WM_APP + 41;
inline constexpr UINT DisplayErrorMessage = WM_APP + 42;
class RmlHost {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    RmlHost(HWND window,std::function<void(Json)> receive,const std::filesystem::path& testDirectory={});
    ~RmlHost();
    void Start(bool software);
    void Resize();
    void Visibility(bool visible);
    void ShowError();
    void Send(const Json& value);
    void Frame();
    bool Message(UINT message,WPARAM w,LPARAM l);
    bool Ready() const;
    bool Software() const;
};
}
