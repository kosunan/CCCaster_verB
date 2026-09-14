#pragma once
#include <cstdlib>
#include <cstdint>

namespace cccaster::testing {
// 実デバイス経由の試験専用。入力値そのものは生成しない。
inline uint32_t VirtualControllerProduct() {
    static const uint32_t product = [] {
        const char *v = std::getenv("CCCASTER_TEST_VIRTUAL_PRODUCT");
        return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 16)) : 0u;
    }();
    return product;
}
inline bool IsVirtualControllerTest() { return VirtualControllerProduct() != 0; }
}
