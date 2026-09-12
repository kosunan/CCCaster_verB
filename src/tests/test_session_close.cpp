#include "tests/test_support.hpp"
#include "shared_contracts/SessionClosePacket.hpp"
using namespace cccaster::public_api;
int main() {
#ifdef _WIN32
    CC_CASE("交渉で設定したWin32環境のnonceを同じランチャーが即座に読める");
    const char *name = "CCCASTER_TEST_CLOSE_NONCE";
    SetEnvironmentVariableA(name, "123456789");
    CC_CHECK_EQ(SessionNonce(name), 123456789);
    SetEnvironmentVariableA(name, "987654321");
    CC_CHECK_EQ(SessionNonce(name), 987654321);
    SetEnvironmentVariableA(name, nullptr);
#endif
    CC_CASE("終了操作の理由とACKを往復し両nonceを照合する");
    for (int reason = 0; reason <= 3; ++reason) {
        for (bool ack : {false, true}) {
            SessionCloseMessage result;
            const auto packet = EncodeSessionClose({static_cast<SessionExitReason>(reason), ack, 123, 456});
            CC_CHECK(DecodeSessionClose(packet, 456, 123, result));
            CC_CHECK_EQ(static_cast<int>(result.reason), reason);
            CC_CHECK_EQ(result.ack, ack);
            CC_CHECK(!DecodeSessionClose(packet, 456, 124, result));
            CC_CHECK(!DecodeSessionClose(packet, 457, 123, result));
            CC_CHECK(!DecodeSessionClose(packet, 0, 123, result));
            for (size_t size = 0; size < packet.size(); ++size)
                CC_CHECK(!DecodeSessionClose({packet.begin(), packet.begin() + size}, 456, 123, result));
            auto extra = packet; extra.push_back(0);
            CC_CHECK(!DecodeSessionClose(extra, 456, 123, result));
            for (size_t index = 0; index < packet.size(); ++index) {
                auto bad = packet; bad[index] = 0xff;
                CC_CHECK(!DecodeSessionClose(bad, 456, 123, result));
            }
        }
    }
    return cccaster::test::Summarize("session_close");
}
