#pragma once
#include <cstdint>
namespace cccaster::core::sync {
// 再戦の確定項目だけを交換する。0=未選択、1=ONCE、2=キャラセレ。
// 次画面でも直前の通知とACKを保持し、遷移直前のパケット損失から回復する。
struct RetrySelection {
    uint32_t epoch = 0, choice = 0, ack = 0;
    bool Valid() const {
        return choice <= 2 && ack <= 2 &&
               (epoch ? epoch % 65536 == 0 : !choice && !ack);
    }
    bool Accept(const RetrySelection &in) {
        if (!in.Valid() || in.epoch < epoch) return false;
        if (in.epoch > epoch) { *this = in; return true; }
        // 一度確定した選択を古い未選択通知や別の選択で上書きしない。
        if (choice && in.choice && choice != in.choice) return false;
        if (ack && in.ack && ack != in.ack) return false;
        if (in.choice) choice = in.choice;
        if (in.ack) ack = in.ack;
        return true;
    }
    int Result(const RetrySelection &peer) const {
        if (!epoch || epoch != peer.epoch) return -1;
        if (choice == 2 || peer.choice == 2) return 1;
        return choice == 1 && peer.choice == 1 ? 0 : -1;
    }
    bool CanRelease(const RetrySelection &peer) const {
        return Result(peer) >= 0 && ack == peer.choice && (!choice || peer.ack == choice);
    }
};
static_assert(sizeof(RetrySelection) == 12);
}
