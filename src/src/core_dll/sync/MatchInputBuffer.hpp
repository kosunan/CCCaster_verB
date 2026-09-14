#pragma once
// ============================================================================
// MatchInputBuffer — 入力のフレーム別バッファ
//
// 【責務】
//   フレーム番号をキーに、自入力（確定）と相手入力（予測 or 確定）を保持する。
//   予測が外れたフレームを記録し、ロールバックの起点として通知する。
//
// 【2スレッドで共有される】
//   ゲームスレッド : 自入力を書く（WriteLocal）、読み出す（TryReadForGame）
//   通信スレッド   : 相手入力を確定する（ConfirmRemote）、送信用に自入力を読む
//
//   両者が同じスロットの同じフィールドを非アトミックに書くと競合する。
//   旧実装はこれを放置しており、通信スレッドが「新しいフレーム番号 + 古い入力値」
//   を読んで誤った過去入力を配っていた（harness で ConfirmConflicts が
//   1900件超を検出）。そのため以下の2点で競合を構造的に無くしている。
//
//   1. レーン分離 — local 系フィールドはゲームスレッドだけ、remote 系は
//      通信スレッドだけが書く。互いのレーンは読むだけ。
//   2. 番号と入力値を64bit atomicの1スナップショットとして公開する。
//      周回中でも番号と値が分離しない。補助メタデータもatomicで共有する。
//
// 【スロットの所在】
//   frame % RING_SIZE。周回すると古いフレームは上書きされるため、
//   読み書きの両方で「そのスロットが本当に要求フレームのものか」を検証する。
//
// 【デシンクの検出】
//   冗長入力により同じフレームが何度も確定される。2回目以降で値が食い違うのは
//   通信の破綻かデシンクであり、黙って上書きしてはいけない。
//   ConfirmConflicts() で観測可能にしている。
// ============================================================================

#include <atomic>
#include <cstdint>

namespace cccaster {
namespace core {
namespace sync {

class MatchInputBuffer {
  public:
    static constexpr int RING_SIZE = 600;              // 10秒分 (60fps × 10s)
    static constexpr uint32_t EMPTY_SLOT = UINT32_MAX; // 未使用（フレーム0と区別する）

    static MatchInputBuffer &GetInstance() {
        static MatchInputBuffer instance;
        return instance;
    }

    // ════════════════════════════════════════════════════
    // ゲームスレッド — 書き込み
    // ════════════════════════════════════════════════════

    /// 自入力を書き込む。predictedRemote はロールバック判定用の予測値。
    void WriteLocal(uint32_t frame, uint32_t localInput, uint32_t predictedRemote, bool rollbackable) {
        Slot &s = _ring[frame % RING_SIZE];
        s.localSnapshot.store((uint64_t(frame) << 32) | localInput, std::memory_order_release);
        s.predictedRemote = predictedRemote;
        s.rollbackable = rollbackable;
        s.localFrame.store(frame, std::memory_order_release); // ここで公開
        _writeHead.store(frame, std::memory_order_release);
    }

    uint32_t GetWriteHead() const {
        return _writeHead.load(std::memory_order_acquire);
    }
    void SetWriteHead(uint32_t frame) {
        _writeHead.store(frame, std::memory_order_release);
    }

    // ════════════════════════════════════════════════════
    // 通信スレッド — 相手入力の確定
    // ════════════════════════════════════════════════════

    void ConfirmRemote(uint32_t frame, uint32_t input) {
        Slot &s = _ring[frame % RING_SIZE];

        if (s.remoteFrame.load(std::memory_order_acquire) == frame) {
            // 冗長入力による再確定。値が食い違うなら通信破綻かデシンク。
            if (uint32_t(s.remoteSnapshot.load(std::memory_order_acquire)) != input) {
                _confirmConflicts.fetch_add(1, std::memory_order_relaxed);
                RecordMismatch(frame);
            }
            return; // 最初の確定値を正とする
        }

        // 遅れて届いた前世代/周回前の入力で新しいスロットを破壊しない。
        const uint32_t existing = s.remoteFrame.load(std::memory_order_acquire);
        if (existing != EMPTY_SLOT && existing > frame)
            return;

        // 自入力が既に書かれていれば、予測が当たっていたか判定できる
        if (s.localFrame.load(std::memory_order_acquire) == frame && s.predictedRemote != input) {
            RecordMismatch(frame);
        }

        s.remoteSnapshot.store((uint64_t(frame) << 32) | input, std::memory_order_release);
        s.remoteFrame.store(frame, std::memory_order_release); // ここで公開

        const uint32_t prev = _confirmedRemoteFrame.load(std::memory_order_relaxed);
        if (prev == EMPTY_SLOT || frame > prev) {
            _confirmedRemoteFrame.store(frame, std::memory_order_release);
        }
    }

    // ════════════════════════════════════════════════════
    // 読み出し（スロットを外に露出しない）
    // ════════════════════════════════════════════════════

    /// ゲームに書き込む入力を取り出す。自入力と相手入力の両方が
    /// readPos のフレームで揃っている必要がある。
    bool TryReadForGame(bool isHost, uint32_t &p1, uint32_t &p2) const {
        const uint32_t readPos = GetReadPos();
        uint32_t local = 0, remote = 0;
        if (!TryGetLocalInput(readPos, local))
            return false;
        if (!TryGetRemoteInput(readPos, remote))
            return false;

        if (isHost) {
            p1 = local;
            p2 = remote;
        } else {
            p1 = remote;
            p2 = local;
        }
        return true;
    }

    /// 送信（冗長入力）用。周回して別フレームになっていれば false。
    bool TryGetLocalInput(uint32_t frame, uint32_t &out) const {
        const Slot &s = _ring[frame % RING_SIZE];
        const uint64_t snapshot = s.localSnapshot.load(std::memory_order_acquire);
        if (uint32_t(snapshot >> 32) != frame)
            return false;
        out = uint32_t(snapshot);
        return true;
    }

    bool TryGetRemoteInput(uint32_t frame, uint32_t &out) const {
        const Slot &s = _ring[frame % RING_SIZE];
        const uint64_t snapshot = s.remoteSnapshot.load(std::memory_order_acquire);
        if (uint32_t(snapshot >> 32) != frame)
            return false;
        out = uint32_t(snapshot);
        return true;
    }

    bool HasLocal(uint32_t frame) const {
        return _ring[frame % RING_SIZE].localFrame.load(std::memory_order_acquire) == frame;
    }
    bool HasRemote(uint32_t frame) const {
        return _ring[frame % RING_SIZE].remoteFrame.load(std::memory_order_acquire) == frame;
    }
    bool IsRollbackable(uint32_t frame) const {
        const Slot &s = _ring[frame % RING_SIZE];
        return s.localFrame.load(std::memory_order_acquire) == frame && s.rollbackable;
    }

    /// 読み出し位置。writeHead から D+R だけ遡る。
    // 期限付き待機経路は消費対象を明示する。旧バッファ単体APIは従来式を維持。
    void SetGameReadFrame(uint32_t frame) {
        _gameReadFrame.store(frame, std::memory_order_release);
    }
    uint32_t GetReadPos() const {
        const auto explicitFrame = _gameReadFrame.load(std::memory_order_acquire);
        if (explicitFrame != EMPTY_SLOT)
            return explicitFrame;
        const uint32_t wh = _writeHead.load(std::memory_order_acquire);
        int32_t offset = static_cast<int32_t>(_delay) + static_cast<int32_t>(_maxRollback);
        if (offset < 1)
            offset = 1; // 最低1フレーム遅延
        const int32_t pos = static_cast<int32_t>(wh) - offset;
        return (pos >= 0) ? static_cast<uint32_t>(pos) : 0;
    }

    bool HasConfirmedRemote() const {
        return _confirmedRemoteFrame.load(std::memory_order_acquire) != EMPTY_SLOT;
    }
    uint32_t GetConfirmedRemoteFrame() const {
        const uint32_t f = _confirmedRemoteFrame.load(std::memory_order_acquire);
        return (f == EMPTY_SLOT) ? 0 : f;
    }

    /// 相手の確定状況で頭打ちになる、実効的な進行可能フレーム。
    uint32_t GetEffectiveHead() const {
        const uint32_t wh = _writeHead.load(std::memory_order_acquire);
        int32_t offset = static_cast<int32_t>(_delay) + static_cast<int32_t>(_maxRollback);
        if (offset < 1)
            offset = 1;

        int32_t delayAdjusted = static_cast<int32_t>(wh) - offset;
        if (delayAdjusted < 0)
            delayAdjusted = 0;
        const uint32_t da = static_cast<uint32_t>(delayAdjusted);

        if (!HasConfirmedRemote())
            return da;
        const uint32_t confirmed = GetConfirmedRemoteFrame();
        return (da < confirmed) ? da : confirmed;
    }

    // ════════════════════════════════════════════════════
    // ミスマッチ（ロールバックの起点）
    // ════════════════════════════════════════════════════

    bool HasMismatch() const {
        return _hasMismatch.load(std::memory_order_acquire);
    }

    /// 最も古いミスマッチフレームを取り出してクリアする。
    bool ConsumeMismatch(uint32_t &frame) {
        if (!_hasMismatch.exchange(false, std::memory_order_acq_rel))
            return false;
        frame = _mismatchFrame.load(std::memory_order_acquire);
        return true;
    }

    /// 確定済みフレームに異なる値が再確定された回数。0 以外ならデシンクを疑う。
    uint32_t ConfirmConflicts() const {
        return _confirmConflicts.load(std::memory_order_relaxed);
    }

    // ════════════════════════════════════════════════════
    // 初期化
    // ════════════════════════════════════════════════════

    void Initialize(uint32_t startFrame, int16_t delay, int16_t maxRollback) {
        Reset();
        SetWriteHead(startFrame);
        SetSyncParams(delay, maxRollback);
    }

    void SetSyncParams(int16_t delay, int16_t maxRollback) {
        _delay = delay;
        _maxRollback = maxRollback;
    }

    int16_t GetDelay() const {
        return _delay;
    }
    int16_t GetMaxRollback() const {
        return _maxRollback;
    }

    /// 進行状態を消す。D/R はセッション固有パラメータなので残す。
    void Reset() {
        for (Slot &s : _ring) {
            s.localFrame.store(EMPTY_SLOT, std::memory_order_relaxed);
            s.remoteFrame.store(EMPTY_SLOT, std::memory_order_relaxed);
            s.localSnapshot.store(uint64_t(EMPTY_SLOT) << 32, std::memory_order_relaxed);
            s.remoteSnapshot.store(uint64_t(EMPTY_SLOT) << 32, std::memory_order_relaxed);
            s.predictedRemote = 0;
            s.rollbackable = false;
        }
        _writeHead.store(0, std::memory_order_relaxed);
        _gameReadFrame.store(EMPTY_SLOT, std::memory_order_relaxed);
        _mismatchFrame.store(0, std::memory_order_relaxed);
        _hasMismatch.store(false, std::memory_order_relaxed);
        _confirmedRemoteFrame.store(EMPTY_SLOT, std::memory_order_relaxed);
        _confirmConflicts.store(0, std::memory_order_relaxed);
    }

  private:
    struct Slot {
        // ── local レーン: ゲームスレッドだけが書く ──
        std::atomic<uint32_t> localFrame{EMPTY_SLOT};
        std::atomic<uint64_t> localSnapshot{uint64_t(EMPTY_SLOT) << 32};
        std::atomic<uint32_t> predictedRemote{0};
        std::atomic<bool> rollbackable{false};

        // ── remote レーン: 通信スレッドだけが書く ──
        std::atomic<uint32_t> remoteFrame{EMPTY_SLOT};
        std::atomic<uint64_t> remoteSnapshot{uint64_t(EMPTY_SLOT) << 32};
    };

    MatchInputBuffer() = default;

    void RecordMismatch(uint32_t frame) {
        if (!_hasMismatch.load(std::memory_order_relaxed)) {
            _mismatchFrame.store(frame, std::memory_order_release);
            _hasMismatch.store(true, std::memory_order_release);
            return;
        }
        // より古いミスマッチを優先する（ロールバックの起点になるため）
        if (frame < _mismatchFrame.load(std::memory_order_relaxed)) {
            _mismatchFrame.store(frame, std::memory_order_release);
        }
    }

    Slot _ring[RING_SIZE];

    std::atomic<uint32_t> _writeHead{0};
    std::atomic<uint32_t> _gameReadFrame{EMPTY_SLOT};

    std::atomic<uint32_t> _mismatchFrame{0};
    std::atomic<bool> _hasMismatch{false};

    std::atomic<uint32_t> _confirmedRemoteFrame{EMPTY_SLOT};
    std::atomic<uint32_t> _confirmConflicts{0};

    int16_t _delay = 0;
    int16_t _maxRollback = 0;
};

} // namespace sync
} // namespace core
} // namespace cccaster
