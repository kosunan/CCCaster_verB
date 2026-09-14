#pragma once
#include <cstdint>
#include "shared_contracts/NetplaySettings.hpp"
namespace cccaster::core::sync {
// キャラセレ専用。フレーム入力ではなく、所有者が確定した値を再送する。
// ackは受信済みrevisionを示す。順序逆転や前回選択の再送では状態を戻さない。
struct SelectionState {
    uint32_t epoch = 0, revision = 0, ack = 0;
    uint32_t selector = 0, character = 0, moon = 0, color = 0;
    uint32_t confirmed = 0, stage = 0, stageConfirmed = 0;
    uint32_t commandSerial = 0, command = 0, commandAck = 0;
    uint32_t delay = 2, rollback = 4;
    static int CharacterCell(uint32_t character) {
        constexpr int chars[] = {22,7,51,15,28,8,2,0,30,11,9,31,4,3,1,19,12,13,14,29,17,18,33,23,10,25,35,5,20,6,34};
        constexpr int cells[] = {2,3,4,5,6,10,11,12,13,14,15,16,19,20,21,22,23,24,25,28,29,30,31,32,33,34,38,39,40,41,42};
        for (unsigned i = 0; i < sizeof(chars)/sizeof(*chars); ++i)
            if (character == uint32_t(chars[i])) return cells[i];
        return -1;
    }
    bool Valid() const {
        return (!epoch || (epoch % 65536 == 0 && revision)) &&
               selector < 54 && character <= 100 && moon < 3 && color < 36 &&
               confirmed <= 1 && (!confirmed || CharacterCell(character) >= 0) &&
               stage < 100 && stageConfirmed <= 1 &&
               (!stageConfirmed || (confirmed && stage != 0)) &&
               public_api::NetplaySettings::IsValid(delay, rollback) &&
               (!command || (((command >> 28) == 0xa || (command >> 28) == 0xb) &&
                              (command & 0x0fffffff) <= 0x08000000 && !(command & 0x00ffffff)));
    }
    bool Accept(const SelectionState &incoming) {
        if (!incoming.Valid() || incoming.epoch < epoch ||
            (incoming.epoch == epoch && incoming.revision < revision)) return false;
        const auto previousAck = incoming.epoch == epoch ? ack : 0;
        *this = incoming;
        if (ack < previousAck) ack = previousAck;
        return true;
    }
    bool PeerHasFinal(const SelectionState &peer) const {
        return epoch && peer.epoch == epoch && confirmed && peer.confirmed && peer.ack >= revision;
    }
};
static_assert(sizeof(SelectionState) == 60);
}
