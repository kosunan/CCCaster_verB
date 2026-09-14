#pragma once
// ============================================================================
// Optimized SFX Filter — SSE2 SIMD + ブランチフリー最適化版
// ============================================================================

#include <cstdint>
#include <cstring>
#include <array>

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <emmintrin.h>  // SSE2
#endif

namespace optimized {

static constexpr size_t SFX_ARRAY_LEN = 1500;
static constexpr size_t NUM_STATES = 256;

// SSE2で16バイト単位処理するため、16の倍数に切り上げ
static constexpr size_t SFX_ALIGNED_LEN = ((SFX_ARRAY_LEN + 15) / 16) * 16; // = 1504

class SfxFilter {
public:
    // 16バイトアライメント確保
    alignas(16) uint8_t filterArray[SFX_ALIGNED_LEN];
    alignas(16) uint8_t muteArray[SFX_ALIGNED_LEN];
    alignas(16) std::array<std::array<uint8_t, SFX_ALIGNED_LEN>, NUM_STATES> sfxHistory;

    SfxFilter() {
        memset(filterArray, 0, SFX_ALIGNED_LEN);
        memset(muteArray, 0, SFX_ALIGNED_LEN);
        for (auto& h : sfxHistory)
            memset(h.data(), 0, SFX_ALIGNED_LEN);
    }

    void SaveCurrentSfx(uint32_t frame) {
        memcpy(&sfxHistory[frame % NUM_STATES][0], filterArray, SFX_ARRAY_LEN);
    }

    // SSE2版: ORマージ + 0x80フラグ設定
    void InitFilterForRollback(uint32_t rollbackFrame, uint32_t origFrame) {
        // ORマージ: 16バイト同時 (1504 / 16 = 94回)
        for (uint32_t i = rollbackFrame + 1; i < origFrame; ++i) {
            const uint8_t* hist = &sfxHistory[i % NUM_STATES][0];
            for (size_t j = 0; j < SFX_ALIGNED_LEN; j += 16) {
                __m128i a = _mm_load_si128(reinterpret_cast<const __m128i*>(filterArray + j));
                __m128i b = _mm_load_si128(reinterpret_cast<const __m128i*>(hist + j));
                _mm_store_si128(reinterpret_cast<__m128i*>(filterArray + j),
                                _mm_or_si128(a, b));
            }
        }

        // 0x80フラグ設定: ブランチフリー SSE2版
        // 非ゼロ → 0x80, ゼロ → 0x00
        const __m128i zero = _mm_setzero_si128();
        const __m128i flag = _mm_set1_epi8(static_cast<char>(0x80));
        for (size_t j = 0; j < SFX_ALIGNED_LEN; j += 16) {
            __m128i v = _mm_load_si128(reinterpret_cast<const __m128i*>(filterArray + j));
            // cmpeq → ゼロの位置が 0xFF、非ゼロが 0x00
            __m128i isZero = _mm_cmpeq_epi8(v, zero);
            // andnot(isZero, flag) → ゼロ位置は0、非ゼロ位置は0x80
            __m128i result = _mm_andnot_si128(isZero, flag);
            _mm_store_si128(reinterpret_cast<__m128i*>(filterArray + j), result);
        }
    }

    // SSE2版: ブランチフリー saveRerunSounds
    void SaveRerunSounds(uint32_t frame) {
        uint8_t* hist = &sfxHistory[frame % NUM_STATES][0];
        const __m128i mask7f = _mm_set1_epi8(0x7F);
        const __m128i zero = _mm_setzero_si128();
        const __m128i one = _mm_set1_epi8(1);

        for (size_t j = 0; j < SFX_ALIGNED_LEN; j += 16) {
            __m128i v = _mm_load_si128(reinterpret_cast<const __m128i*>(filterArray + j));
            // v & 0x7F → 下位7ビットのみ
            __m128i masked = _mm_and_si128(v, mask7f);
            // masked != 0 → isNonZero = 0xFF
            __m128i isZero = _mm_cmpeq_epi8(masked, zero);
            // andnot(isZero, one) → 非ゼロなら1、ゼロなら0
            __m128i result = _mm_andnot_si128(isZero, one);
            _mm_store_si128(reinterpret_cast<__m128i*>(hist + j), result);
        }
    }

    // SSE2版: finishedRerunSounds
    void FinishedRerunSounds(uint8_t* gameSfxArray) {
        const __m128i flag80 = _mm_set1_epi8(static_cast<char>(0x80));
        const __m128i one = _mm_set1_epi8(1);

        for (size_t j = 0; j < SFX_ALIGNED_LEN; j += 16) {
            __m128i v = _mm_load_si128(reinterpret_cast<const __m128i*>(filterArray + j));
            // v == 0x80 の位置を検出
            __m128i match = _mm_cmpeq_epi8(v, flag80);

            // gameSfxArray[j] |= (match ? 1 : 0)
            __m128i game = _mm_loadu_si128(reinterpret_cast<const __m128i*>(gameSfxArray + j));
            __m128i setOne = _mm_and_si128(match, one);
            game = _mm_or_si128(game, setOne);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(gameSfxArray + j), game);

            // muteArray[j] |= (match ? 1 : 0)
            __m128i mute = _mm_load_si128(reinterpret_cast<const __m128i*>(muteArray + j));
            mute = _mm_or_si128(mute, setOne);
            _mm_store_si128(reinterpret_cast<__m128i*>(muteArray + j), mute);
        }
        memset(filterArray, 0, SFX_ALIGNED_LEN);
    }
};

} // namespace optimized
