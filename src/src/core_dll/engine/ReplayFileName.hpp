#pragma once
#include <string>
#include <string_view>
#include <array>

namespace cccaster::domain::session {
inline std::array<std::string, 2> ReplayFilePlayerNames(std::string_view path) {
    std::array<std::string, 2> result{};
    const auto slash = path.find_last_of("/\\");
    auto name = path.substr(slash == path.npos ? 0 : slash + 1);
    if (!name.ends_with(".rep")) return result;
    name.remove_suffix(4);
    const auto first = name.find("_P1-");
    if (first == name.npos) return result;
    name.remove_prefix(first + 4);
    const auto winBoundary = name.find("[WIN]_P2-");
    const auto second = winBoundary != name.npos ? winBoundary + 5 : name.find("_P2-");
    if (second == name.npos) return result;
    auto p1 = name.substr(0, second), p2 = name.substr(second + 4);
    if (p1.ends_with("[WIN]")) p1.remove_suffix(5);
    const auto mark = p2.find('[');
    if (mark != p2.npos) {
        const bool undecided = p2.substr(mark).starts_with("[UNDECIDED]");
        p2 = p2.substr(0, mark);
        if (undecided && p2.ends_with('_')) p2.remove_suffix(1);
    }
    if (p1.empty() || p2.empty() || p1.size() > 31 || p2.size() > 31) return result;
    result[0] = p1; result[1] = p2;
    return result;
}
inline std::string ReplayPlayerName(std::string_view name, const char *fallback) {
    std::string result;
    for (unsigned char c : name.substr(0, 31)) {
        if (c < 32 || c == 127 || std::string_view("<>:\"/\\|?*[]").find(char(c)) != std::string_view::npos)
            result += '_';
        else result += char(c);
    }
    while (!result.empty() && (result.back() == ' ' || result.back() == '.')) result.pop_back();
    return result.empty() ? fallback : result;
}
// winner=0は未裁定。推測でどちらかを勝者にしない。
inline std::string ReplayFileStem(std::string_view timestamp, std::string_view p1,
                                  std::string_view p2, int winner) {
    return std::string(timestamp) + "_P1-" + ReplayPlayerName(p1, "PLAYER1") +
        (winner == 1 ? "[WIN]" : "") + "_P2-" + ReplayPlayerName(p2, "PLAYER2") +
        (winner == 2 ? "[WIN]" : "") + (winner == 1 || winner == 2 ? "" : "_[UNDECIDED]");
}
}
