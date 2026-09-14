#include "shared_contracts/StartupNegotiation.hpp"
#include "tests/test_support.hpp"
using namespace cccaster::public_api::startup;
int main() {
    CC_CASE("旧形式と拡張形式の厳密な長さ・識別子");
    Probe legacy, decoded;
    legacy.timestamp = 123;
    CC_CHECK(Decode(Encode(legacy), decoded));
    CC_CHECK(!decoded.extended);
    CC_CHECK_EQ(decoded.timestamp, 123);
    Probe a;
    a.extended = true; a.nonce = 11; a.timestamp = 456;
    auto bytes = Encode(a);
    CC_CHECK_EQ(bytes.size(), 46);
    CC_CHECK(Decode(bytes, decoded));
    CC_CHECK_EQ(decoded.nonce, 11);
    bytes[26] ^= 1;
    CC_CHECK(!Decode(bytes, decoded));
    CC_CHECK(!Decode(std::vector<uint8_t>(45), decoded));
    CC_CHECK(!Decode(std::vector<uint8_t>(47), decoded));
    CC_CHECK(!Decode(std::vector<uint8_t>(20), decoded));
    a.nonce = 0;
    CC_CHECK(!Decode(Encode(a), decoded));
    a.nonce = 11;

    CC_CASE("双方のnonce確認と返信送信を待つ");
    Agreement host{22}, client{11};
    CC_CHECK(host.Receive(a));
    CC_CHECK(!host.Ready());
    Probe b;
    b.extended = true; b.nonce = 22; b.echoNonce = 11; b.timestamp = 789;
    host.Sent(b.echoNonce);
    CC_CHECK(!host.Ready());
    CC_CHECK(client.Receive(b));
    CC_CHECK(!client.Ready());
    a.echoNonce = 22;
    client.Sent(a.echoNonce);
    CC_CHECK(client.Ready());

    CC_CASE("最後の確認喪失をDLLで再応答");
    // clientの最後のaが喪失。hostはbを再送し、移管済みclient DLLが返信する。
    CC_CHECK(CanReply(b, 11, 22, 1000, false));
    Probe recovered = a;
    CC_CHECK(host.Receive(recovered));
    CC_CHECK(host.Ready());
    CC_CHECK(!CanReply(b, 11, 23, 1000, false));
    CC_CHECK(!CanReply(b, 12, 22, 1000, false));
    CC_CHECK(!CanReply(b, 11, 22, 30000000, false));
    CC_CHECK(!CanReply(b, 11, 22, -1, false));
    CC_CHECK(!CanReply(b, 11, 22, 1000, true));
    CC_CHECK(!CanReply(legacy, 11, 22, 1000, false));
    auto dllReply = b; dllReply.timestamp = 0;
    CC_CHECK(!CanReply(dllReply, 11, 22, 1000, false));
    Agreement waitingClient{11};
    CC_CHECK(waitingClient.Receive(dllReply));
    waitingClient.Sent(22);
    CC_CHECK(waitingClient.Ready());

    CC_CASE("別世代と無関係なechoを拒否");
    auto wrong = b; wrong.nonce = 33;
    CC_CHECK(!client.Receive(wrong));
    wrong = b; wrong.echoNonce = 99;
    CC_CHECK(!client.Receive(wrong));
    CC_CHECK(client.Ready());
    Agreement oldPeer{11};
    CC_CHECK(oldPeer.Receive(legacy));
    CC_CHECK(!oldPeer.Ready());
    CC_CASE("localhostの自己probeでは合意しない");
    Agreement self{11};
    a.nonce = 11; a.echoNonce = 11;
    CC_CHECK(!self.Receive(a));
    self.Sent(11);
    CC_CHECK(!self.Ready());
    CC_CHECK_EQ(self.peerNonce, 0);
    return cccaster::test::Summarize("startup_negotiation");
}
