#pragma once
#include <cstdlib>
namespace cccaster::testing {
inline bool IsInputTraceEnabled() {
    static const bool enabled = [] {
        const char *v = std::getenv("CCCASTER_INPUT_TRACE");
        return v && v[0] == '1';
    }();
    return enabled;
}
} // namespace cccaster::testing
