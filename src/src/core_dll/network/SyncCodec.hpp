#pragma once
// ============================================================================
// SyncCodec — 同期計算器
//
// 【責務】
//   - 受信パケット解析 → Θ/RTT 計算（NetplayClock 利用）
//   - 採用時計の周期補正をMetronomeへ供給（位相追従はInputTimeline）
//   - FrameInputBuffer 書込み（入力データ）
//   - 送信パケット組立て（SYNC_TICK のみ）
//   - D/R dirty 管理
//
// 【パケット設計】
//   全フェーズ（WaitReady/WaitStart/Counting）で SYNC_TICK 1種類のみ使用。
//   フェーズの違いは flags と startTimeTicks フィールドで表現する。
//
// 【スレッド安全性】
//   ProcessReceivedPacket() は通信スレッドから呼ばれる。
//   周期補正は Metronome に atomic 経由で供給。
// ============================================================================

#include <cstdint>
#include <atomic>
#include <vector>
#include <string>
#include "core_dll/sync/NetplayClock.hpp"

namespace cccaster {
namespace core {
namespace netplay {

class Metronome; // 前方宣言

class SyncCodec {
  public:
    // ─── 初期化 ─────────────────────────────────────────
    void Initialize(bool isHost, int delayFrames, int maxRollback, Metronome *metronome);
    void Reset();

    // ─── 受信パケット処理 ────────────────────────────────
    // 通信スレッドから呼ばれる。パケット解析・Θ計算・FrameInputBuffer書込みを行う。
    void ProcessReceivedPacket(const std::vector<uint8_t> &data, const std::string &fromIp, uint16_t fromPort,
                               int64_t receiveTimeTicks);

    // ─── 送信パケット組立て ──────────────────────────────
    /// 全フェーズ共通の GAME_TICK パケットを構築する。
    /// @param frame     フレーム番号 (Counting時のみ有効、それ以外は0)
    /// @param localInput ローカル入力 (Counting時のみ有効)
    /// @param ready     true: 準備完了シグナル (WaitReady/WaitStart)
    /// @param startTimeUs メトロノーム開始時刻 (WaitStart時のみ有効、0=未設定)
    std::vector<uint8_t> BuildPacket(uint32_t frame, uint32_t localInput, bool ready = false,
                                     int64_t startTimeUs = 0, bool latestInputWindow = false);
    uint32_t RepairWindowEnd(uint32_t frame) const;
    // 通信スレッド所有のバッファへ構築。容量を再利用し、毎パケットの確保を省く。
    void BuildPacketInto(std::vector<uint8_t> &packet, uint32_t frame, uint32_t localInput,
                         bool ready = false, int64_t startTimeUs = 0, bool latestInputWindow = false);

    // ─── 採用時計の周期補正を更新 ────────────────────────────────────
    void UpdateAlphaCorrections();

    // ─── D/R 動的変更 ───────────────────────────────────
    void SetDelayFrames(int d) {
        if (!_configurationLocked.load()) {
            _delayFrames = d;
            _delayDirty = true;
        }
    }
    void SetMaxRollback(int r) {
        if (!_configurationLocked.load()) {
            _maxRollback = r;
            _rollbackDirty = true;
        }
    }
    int GetDelayFrames() const {
        return _delayFrames;
    }
    int GetMaxRollback() const {
        return _maxRollback;
    }

    // ─── 時計データ読取り（オーバーレイ用）───────────────
    uint64_t GetRttSampleSerial() const { return _clock.GetRttSampleSerial(); }
    int64_t GetLatestRttUs() const { return _clock.GetLatestRttUs(); }
    int64_t GetRttUs() const {
        return _clock.GetRttUs();
    }
    int64_t GetThetaUs() const {
        return _clock.GetThetaUs();
    }
    int64_t GetBaselineTheta() const {
        return _clock.GetBaselineTheta();
    }
    bool IsThetaStable() const {
        return _clock.IsThetaStable();
    }

    bool HasTimingEstimate() const {
        return _clock.HasTimingEstimate();
    }

    // ─── スタート時刻管理（ハンドシェイク用）────────────
    void SetLocalStartTime(int64_t us) {
        _clock.SetLocalStartTime(us);
    }
    void SetPeerStartTime(int64_t us) {
        _clock.SetPeerStartTime(us);
    }
    int64_t GetAgreedStartTime() const {
        return _clock.GetAgreedStartTime();
    }
    void SetBaselineTheta() {
        _clock.SetBaselineTheta();
    }

    // ─── 疎通管理 ───────────────────────────────────────
    bool IsPeerAlive() const {
        return !_peerClosed && _framesSinceLastRecv < DISCONNECT_TIMEOUT_FRAMES;
    }
    bool IsPeerClosed() const { return _peerClosed; }
    bool IsPeerReady() const {
        return _peerReady;
    }
    void IncrementFrameCount() {
        _framesSinceLastRecv++;
    }
    uint32_t GetLatestPeerFrame() const {
        return _latestPeerFrame;
    }

    // ─── 定数 ──────────────────────────────────────────
    static constexpr int DISCONNECT_TIMEOUT_FRAMES = 180;

    // 統一ヘッダ
    static constexpr int HDR_TIMESTAMP_OFFSET = 8;
    static constexpr int UNIFIED_HEADER_SIZE = 20;
    static constexpr uint32_t CC10_MAGIC = 0x30314343u;

    // パケットタイプ（GAME_TICK のみ）
    static constexpr uint8_t PKT_SYNC_TICK = 0x30;

    // SyncPayload flags
    //   宛先バッファを指定するフラグ (FLAG_BUFFER_MENU/MATCH) は廃止した。
    //   パケットが受信側の内部データ構造を指名する設計だったため、
    //   フェーズ認識が両者でずれた瞬間に別々のバッファへ振り分けられていた。
    //   入力はセッション通しの単一フレーム空間で扱う。
    static constexpr uint8_t FLAG_READY = 0x01;       // 準備完了
    static constexpr uint8_t FLAG_PHASE_READY = 0x02; // Phase遷移準備完了

  private:
    static void BuildUnifiedPacket(std::vector<uint8_t> &pkt, uint8_t phase, uint8_t type, int64_t timestampTicks,
                                                   const void *payload = nullptr, size_t payloadSize = 0);

    // ─── 計算エンジン ──────────────────────────────────
    timer::NetplayClock _clock;
    uint32_t _lastModelEvaluation = UINT32_MAX;
    uint32_t _localClockGeneration = 0, _peerClockGeneration = UINT32_MAX;
    int64_t _clockValidAfterTicks = 0;
    Metronome *_metronome = nullptr;

    // ─── 構成 ──────────────────────────────────────────
    std::atomic<bool> _configurationLocked{false};
    bool _isHost = false;
    int _delayFrames = 0;
    int _maxRollback = 0;
    bool _delayDirty = false;
    bool _rollbackDirty = false;

    // ─── NTP T1-T4 エコー追跡 ─────────────────────────
    int64_t _lastPeerT1 = 0;
    int64_t _lastPeerRecvTicks = 0;

    // ─── 疎通管理 ──────────────────────────────────────
    bool _peerReady = false;
    bool _peerClosed = false;
    int _framesSinceLastRecv = 0;
    uint32_t _latestPeerFrame = 0;
    uint64_t _peerProgressToken = 0;
    uint32_t _peerAppliedFrame = 0;
    int64_t _peerScheduleSentTicks = 0;
    int64_t _peerProgressReceivedTicks = 0, _peerProgressSentTicks = 0;
};

} // namespace netplay
} // namespace core
} // namespace cccaster
