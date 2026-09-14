#pragma once
#include "core_dll/hook/DirectInputHook.hpp"
#include <array>
#include <string>
#include <span>

namespace cccaster::input {
inline constexpr int BindingCount = 15;
inline constexpr int SaveStateBinding = 13, LoadStateBinding = 14;
using Bindings = std::array<std::string, BindingCount>;
inline constexpr const char *BindingKeys[] = {
    "Up", "Down", "Left", "Right", "A", "B", "C", "D", "E", "Start", "FN1", "FN2", "A+B",
    "TrainingSave", "TrainingLoad"};
inline constexpr const char *BindingLabels[] = {
    "Up", "Down", "Left", "Right", "A / Confirm", "B / Cancel", "C", "D", "E", "Start",
    "FN1 Save_state", "FN2 Load_state", "A+B", "Save state", "Load state"};
inline Bindings DefaultBindings(bool keyboard) {
    if (keyboard)
        return {"I", "K", "J", "L", "S", "D", "F", "G", "E", "LeftShift", "T", "W", "R", "", ""};
    return {"H0_8", "H0_2", "H0_4", "H0_6", "B0", "B1", "B2", "B3", "B4", "B7", "B8", "B9", "", "", ""};
}
inline std::string SafeDeviceName(std::string name) {
    for (char &c : name)
        if (std::string("\\/:*?\"<>|").find(c) != std::string::npos) c = '_';
    return name;
}
struct DeviceIdentity {
    std::string name, guid;
    bool operator==(const DeviceIdentity &) const = default;
    bool Empty() const { return name.empty(); }
};
inline int ResolveDevice(const DeviceIdentity &identity, std::span<const game_interface::JoyDeviceInfo> devices) {
    if (identity.Empty()) return -1;
    if (identity.name == "Keyboard") return -2;
    if (!identity.guid.empty()) {
        for (const auto &device : devices)
            if (identity.guid == device.instanceGuid) return device.id;
        return -1; // 抜けた個体を同型の別個体へ勝手に置き換えない。
    }
    int result = -1, count = 0;
    for (const auto &device : devices)
        if (identity.name == SafeDeviceName(device.name)) { result = device.id; ++count; }
    return count == 1 ? result : -1;
}
inline DeviceIdentity IdentifyDevice(int id, std::span<const game_interface::JoyDeviceInfo> devices) {
    if (id == -2) return {"Keyboard", ""};
    for (const auto &device : devices)
        if (device.id == id) return {SafeDeviceName(device.name), device.instanceGuid};
    return {};
}
inline std::string ProfileFilename(const DeviceIdentity &device, bool legacy = false) {
    return SafeDeviceName(device.name) + (legacy || device.guid.empty() ? "" : "__" + device.guid) + ".ini";
}
} // namespace cccaster::input
