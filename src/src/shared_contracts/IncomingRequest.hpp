#pragma once
#include <set>
#include <string>
#include <string_view>

namespace cccaster::notification {
inline constexpr std::string_view IncomingPrefix = "[INCOMING_REQUEST] ";

// GUIが同じログを読み直しても、一つの受信要求を繰り返し通知しない。
// 現行P2PのAdmissionは待受一回につき最大20要求。新しい待受でResetする。
class IncomingRequests {
    std::set<std::string> seen_;
public:
    unsigned Observe(std::string_view log) {
        unsigned added = 0;
        for (size_t start = 0, end; (end = log.find('\n', start)) != std::string_view::npos;
             start = end + 1) {
            auto line = log.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (!line.starts_with(IncomingPrefix)) continue;
            auto id = line.substr(IncomingPrefix.size());
            if (id.size() != 16 || id.find_first_not_of("0123456789abcdef") != std::string_view::npos)
                continue;
            if (seen_.insert(std::string(id)).second) ++added;
        }
        return added;
    }
    void Reset() { seen_.clear(); }
};
} // namespace cccaster::notification
