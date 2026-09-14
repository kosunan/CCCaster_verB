#pragma once
// ============================================================================
// Legacy SFX Filter — 旧DllRollbackManagerのSFXフィルタロジックを
// ゲーム非依存に切り出したもの。性能ベースライン用。
// ============================================================================

#include <cstdint>
#include <cstring>
#include <array>

namespace legacy {

static constexpr size_t SFX_ARRAY_LEN = 1500;
static constexpr size_t NUM_STATES = 256;

class SfxFilter {
public:
    uint8_t filterArray[SFX_ARRAY_LEN];
    uint8_t muteArray[SFX_ARRAY_LEN];
    std::array<std::array<uint8_t, SFX_ARRAY_LEN>, NUM_STATES> sfxHistory;

    SfxFilter() {
        memset(filterArray, 0, SFX_ARRAY_LEN);
        memset(muteArray, 0, SFX_ARRAY_LEN);
        for (auto& h : sfxHistory)
            memset(h.data(), 0, SFX_ARRAY_LEN);
    }

    // 旧実装L119-120: 毎フレームの履歴保存
    void SaveCurrentSfx(uint32_t frame) {
        memcpy(&sfxHistory[frame % NUM_STATES][0], filterArray, SFX_ARRAY_LEN);
    }

    // 旧実装L210-222: ロールバック時のSFXフィルタ初期化
    void InitFilterForRollback(uint32_t rollbackFrame, uint32_t origFrame) {
        // ORマージループ (旧L210-213)
        for (uint32_t i = rollbackFrame + 1; i < origFrame; ++i) {
            for (uint32_t j = 0; j < SFX_ARRAY_LEN; ++j) {
                filterArray[j] |= sfxHistory[i % NUM_STATES][j];
            }
        }
        // 0x80フラグ設定 (旧L218-222)
        for (uint32_t j = 0; j < SFX_ARRAY_LEN; ++j) {
            if (filterArray[j])
                filterArray[j] = 0x80;
        }
    }

    // 旧実装L232-243: 再計算中のSFX履歴更新
    void SaveRerunSounds(uint32_t frame) {
        uint8_t* hist = &sfxHistory[frame % NUM_STATES][0];
        for (uint32_t j = 0; j < SFX_ARRAY_LEN; ++j) {
            if (filterArray[j] & ~0x80)
                hist[j] = 1;
            else
                hist[j] = 0;
        }
    }

    // 旧実装L246-262: ロールバック完了後のSFX処理
    void FinishedRerunSounds(uint8_t* gameSfxArray) {
        for (uint32_t j = 0; j < SFX_ARRAY_LEN; ++j) {
            if (filterArray[j] == 0x80) {
                gameSfxArray[j] = 1;
                muteArray[j] = 1;
            }
        }
        memset(filterArray, 0, SFX_ARRAY_LEN);
    }
};

} // namespace legacy
