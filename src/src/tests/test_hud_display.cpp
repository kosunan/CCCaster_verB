#include "core_dll/ui/HudDisplay.hpp"
#include "shared_contracts/PlayerName.hpp"
#include <cstdio>
#include <cstring>

int main() {
    using namespace cccaster::domain::ui;
    int failures = 0;
    auto check = [&](bool pass, const char *label) {
        if (!pass) { std::fprintf(stderr, "FAIL: %s\n", label); ++failures; }
    };
    check(HudDisplay::Get() == HudDisplayMode::Compact, "default compact");
    HudDisplay::Cycle();
    check(HudDisplay::Get() == HudDisplayMode::Detailed, "detailed");
    HudDisplay::Cycle();
    check(HudDisplay::Get() == HudDisplayMode::Hidden, "hidden");
    HudDisplay::Cycle();
    check(HudDisplay::Get() == HudDisplayMode::Compact, "wrap compact");
    char line[256];
    FormatNetplayHudLine(line, sizeof(line), true, false, 12.4f, true, 1.25f,
                         2, 4, 16667);
    check(std::strcmp(line,
          "RTT    12ms   JITTER    1.2ms   D 2F   RB LIMIT 4F   1F   16667us") == 0,
          "single-line measured metrics");
    FormatNetplayHudLine(line, sizeof(line), false, false, 0.0f, false, 0.0f,
                         0, 8, 0);
    check(std::strcmp(line,
          "RTT      --   JITTER       --   D 0F   RB LIMIT 8F   1F        --") == 0,
          "single-line unmeasured metrics");
    FormatNetplayHudLine(line, sizeof(line), true, true, 12.0f, true, 1.0f,
                         3, 2, 16666);
    check(std::strcmp(line,
          "RTT   STALE   JITTER    STALE   D 3F   RB LIMIT 2F   1F   16666us") == 0,
          "single-line stale metrics");
    check(std::strstr(ControllerSetupGuidance(false), "PRESS F4") != nullptr &&
          std::strstr(ControllerSetupGuidance(false), "BEFORE SELECTING") != nullptr,
          "character select persistently explains when and how to configure");
    check(std::strstr(ControllerSetupGuidance(true), "SETTINGS KEPT") != nullptr &&
          std::strstr(ControllerSetupGuidance(true), "TEST MOVEMENT") != nullptr,
          "controller close confirmation asks player to test input");
    char carryLine[256];
    FormatNetplayHudLine(line, sizeof(line), true, false, 9.4f, true, 9.94f,
                         2, 4, 9999);
    FormatNetplayHudLine(carryLine, sizeof(carryLine), true, false, 10.4f, true, 10.04f,
                         2, 4, 10000);
    check(std::strlen(line) == std::strlen(carryLine),
          "digit carry keeps total width");
    check(std::strstr(line, "JITTER") - line == std::strstr(carryLine, "JITTER") - carryLine &&
          std::strstr(line, "RB LIMIT") - line == std::strstr(carryLine, "RB LIMIT") - carryLine &&
          std::strstr(line, "1F") - line == std::strstr(carryLine, "1F") - carryLine,
          "digit carry keeps column offsets");
    FormatNetplayHudLine(line, sizeof(line), true, false, 1000000.0f, true, 100000.0f,
                         99, -1, 999999999);
    check(std::strcmp(line,
          "RTT 99999ms   JITTER 9999.9ms   D ?F   RB LIMIT ?F   1F 9999999us") == 0,
          "out-of-range values stay inside fixed fields");
    FormatBattleHudLine(line, sizeof(line), true, false, 9.4f, true, 9.94f,
                        2, 4, 9999, false);
    FormatBattleHudLine(carryLine, sizeof(carryLine), true, false, 10.4f, true, 10.04f,
                        2, 4, 10000, false);
    check(std::strlen(line) == std::strlen(carryLine) &&
          std::strstr(line, "JIT") - line == std::strstr(carryLine, "JIT") - carryLine &&
          std::strstr(line, "1F") - line == std::strstr(carryLine, "1F") - carryLine,
          "bottom battle line keeps fixed columns");
    char playerName[cccaster::public_api::PlayerNameSize];
    cccaster::public_api::NormalizePlayerName(playerName, "  Alice\x01  ", "PLAYER 1");
    check(std::strcmp(playerName, "Alice") == 0, "player name strips controls and surrounding spaces");
    cccaster::public_api::NormalizePlayerName(playerName, "", "PLAYER 2");
    check(std::strcmp(playerName, "PLAYER 2") == 0, "empty player name uses role fallback");
    HudShortcutLatch keys;
    keys.Update(true, true, false, true);
    check(keys.control && keys.f3, "chord suppressed");
    keys.Update(false, true, false, true);
    check(!keys.control && keys.f3, "control released first");
    keys.Update(false, false, false, true);
    check(!keys.control && !keys.f3, "both released");
    keys.Update(true, true, false, true);
    keys.Update(true, false, false, true);
    check(keys.control && !keys.f3, "F3 released first");
    keys.Update(false, false, false, true);
    keys.Update(true, true, true, true);
    check(!keys.control && !keys.f3, "AltGr not HUD shortcut");
    keys.Update(true, true, false, false);
    check(!keys.control && !keys.f3, "background ignored");
    keys.Update(false, true, false, true);
    check(!keys.control && !keys.f3, "ordinary F3 allowed");
    return failures ? 1 : 0;
}
