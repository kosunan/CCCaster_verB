#pragma once
#include "GuiProtocol.hpp"

namespace cccaster::gui {
inline void SortPublicPlayers(Json& people) {
    // 同時刻でも受信順に左右されない。時刻不明の旧登録は末尾に置く。
    std::sort(people.begin(), people.end(), [](const Json& a, const Json& b) {
        const auto left = a.value("listedAt", int64_t(0)), right = b.value("listedAt", int64_t(0));
        return left != right ? left > right : a.at("id") < b.at("id");
    });
}
inline std::string PublicListingAge(int64_t listedAt, int64_t now) {
    if (listedAt <= 0) return "--:--";
    // 相手の時計が進んでいても負の時間を表示しない。
    const auto minutes = now > listedAt ? (now - listedAt) / 60 : 0;
    return std::to_string(minutes / 60) + ":" + (minutes % 60 < 10 ? "0" : "") + std::to_string(minutes % 60);
}
}
