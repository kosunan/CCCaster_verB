#include "core_dll/sync/EpochStart.hpp"
#include "core_dll/sync/RetrySelection.hpp"
#pragma once
// ============================================================================
// NetplaySession — 通信スレッド統括
//
// 【責務】
//   パケットの送受信に専念する。
//   パケット解析・Θ計算・α補正・FrameInputBuffer操作は SyncCodec に委譲。
//   フレームリズム生成は Metronome に委譲。
//
// 【モード遷移】
//   WaitReady  → (双方READY) → WaitStart → (θ安定+合意時刻到達) → Counting
//
// 【スレッド間ルール】
//   - 通信スレッドは送受信・SyncCodec、専用時計スレッドはInputTimelineの採取を担当。
//   - InputTimelineはゲームのPresentと独立したWASAPI締切で進む。
//   - DLLスレッド（ゲームスレッド）は FrameInputBuffer を監視するだけ。
// ============================================================================

#include <atomic>
#include "core_dll/timing/ThreadSignal.hpp"
#include <cstdint>
#include <thread>
#include <vector>
#include <string>
#include <mutex>
#include "core_dll/mbaa_mem/RngState.hpp"
#include "core_dll/sync/SelectionState.hpp"
#include "core_dll/network/SyncCodec.hpp"
#include "core_dll/timing/Metronome.hpp"
#include "shared_contracts/PlayerName.hpp"

namespace cccaster {
namespace core {
namespace netplay {

// ============================================================================
// SyncMode — 通信スレッドの状態
// ============================================================================
enum class SyncMode {
    WaitReady, // 準備完了待機 — READY信号を送り、相手のREADYを待つ
    WaitStart, // 開始時刻待機 — θ推定→START送受信→合意時刻到達を待つ
    Counting   // フレームカウント中 — パケット送受信 + FrameInputBuffer書込み
};

// ============================================================================
// SharedSyncState — スレッド間共有データ
// ============================================================================
struct SharedSyncState {
    std::mutex playerNameMutex;
    char localPlayerName[cccaster::public_api::PlayerNameSize]{};
    char peerPlayerName[cccaster::public_api::PlayerNameSize]{};
    std::mutex retryMutex;
    sync::RetrySelection localRetry{}, peerRetry{};
    std::atomic<uint32_t> retryPreviousFrame{0};
    std::mutex epochStartMutex;
    sync::EpochStart localEpochStart{}, peerEpochStart{};
    int64_t peerEpochStartLocalTicks = 0;
    std::mutex selectionMutex;
    sync::SelectionState localSelection{}, peerSelection{};
    // ─── ティックマスター ───────────────────────────────
    std::atomic<uint32_t> currentFrame{0};
    std::atomic<int64_t> currentTickUs{16666};

    // ─── 同期状態フラグ ─────────────────────────────────
    std::atomic<bool> isSynced{false};
    std::atomic<bool> isPeerAlive{false};
    std::atomic<bool> peerReady{false};
    std::atomic<int64_t> clockOffsetUs{0};
    std::atomic<int64_t> lastRttUs{0};

    // ─── IntroBarrier / Phase遷移同期（ゲーム↔通信スレッド間）───
    std::atomic<bool> localPhaseReady{false}; // ゲームスレッドが設定
    std::atomic<bool> peerPhaseReady{false};  // 通信スレッドが設定（受信時）

    // ─── Phase 遷移同期 ─────────────────────────────────
    std::atomic<uint32_t> phaseBaseFrame{0};     // InGame 開始時の wh 基準点
    std::atomic<uint32_t> peerPhaseBaseFrame{0}; // Peer の基準フレーム

    std::atomic<uint8_t> localPhaseKind{0}, peerPhaseKind{0};
    std::atomic<uint64_t> localPhaseToken{0}, peerPhaseToken{0};
    std::atomic<uint32_t> consumedFrame{0}, peerConsumedFrame{0};
    std::atomic<uint32_t> appliedFrame{0}; // 巻き戻し中も後退しない適用済み先端
    // 入力枠と締切は一組で公開する。相手の締切はローカル時計へ換算済み。
    struct InputSchedule {
        uint32_t base = 0, frame = 0;
        int64_t dueTicks = 0, stampTicks = 0;
        int64_t thetaTicks = 0, rttTicks = 0; // 診断専用。
        int64_t periodCorrectionParts = 0;
        bool modelReady = false;
        uint32_t modelRevision = 0;
    };
    std::mutex scheduleMutex;
    InputSchedule localSchedule{}, peerSchedule{};
    std::atomic<bool> protocolError{false};
    // 世代入口だけゲームスレッドが採取/適用する。通信スレッドはコピーのみ。
    std::mutex seedMutex;
    cccaster::game_interface::RngState localSeed{}, peerSeed{};
    uint32_t localSeedEpoch = 0, peerSeedEpoch = 0;

    // ─── Keepalive 要求（ゲーム→通信スレッド）─────────────────
    std::atomic<bool> needKeepalive{true}; // CB書込みしないPhaseで true
};

// ============================================================================
// 受信パケットエントリ
// ============================================================================
struct ReceivedPacket {
    std::vector<uint8_t> data;
    std::string fromIp;
    uint16_t fromPort;
    int64_t receiveTimeTicks;
};

// ============================================================================
// NetplaySession 本体 — 通信専用
// ============================================================================
class NetplaySession {
  public:
    static NetplaySession &GetInstance();
    static const SharedSyncState &GetState() {
        return GetInstance()._state;
    }
    static SharedSyncState &GetMutableState() {
        return GetInstance()._state;
    }

    // ─── ライフサイクル ─────────────────────────────────
    void Start(bool isHost, const std::string &targetIp, uint16_t targetPort, uint16_t localPort,
               int delayFrames, int maxRollback, const char *playerName);
    void Stop();
    void WakeInputClock() {
        _inputWake.Notify();
    }
    void NotifyInputReady() {
        _networkWake.Notify();
    }
    bool IsRunning() const {
        return _running.load();
    }

    /// @brief メトロノームへのアクセサ（DLLスレッドから WaitForNextTick 用）
    Metronome &GetMetronome() {
        return _metronome;
    }

    // ─── 時計データ読取り（オーバーレイ用、SyncCodec 委譲）──
    int64_t GetRttUs() const {
        return _calc.GetRttUs();
    }
    int64_t GetThetaUs() const {
        return _calc.GetThetaUs();
    }
    int64_t GetBaselineTheta() const {
        return _calc.GetBaselineTheta();
    }
    uint32_t GetLatestPeerFrame() const {
        return _calc.GetLatestPeerFrame();
    }

    // ─── D/R 動的変更（SyncCodec 委譲）─────────────
    void SetDelayFrames(int d) {
        _calc.SetDelayFrames(d);
    }
    void SetMaxRollback(int r) {
        _calc.SetMaxRollback(r);
    }

    // ─── 受信パケットキュー ─────────────────────────────
    void OnPacketReceived(const std::vector<uint8_t> &data, const std::string &fromIp, uint16_t fromPort);

    // ─── 定数 ──────────────────────────────────────────
    static constexpr int KEEPALIVE_INTERVAL_FRAMES = 3; // 3フレーム(≈50ms)ごとに keepalive
    static constexpr int64_t START_MARGIN_US = 500000;

    // パケット定数
    static constexpr int UNIFIED_HEADER_SIZE = SyncCodec::UNIFIED_HEADER_SIZE;
    static constexpr uint32_t CC10_MAGIC = SyncCodec::CC10_MAGIC;
    static constexpr uint8_t PKT_SYNC_TICK = SyncCodec::PKT_SYNC_TICK;

  private:
    NetplaySession() = default;
    ~NetplaySession() {
        Stop();
    }
    NetplaySession(const NetplaySession &) = delete;
    NetplaySession &operator=(const NetplaySession &) = delete;

    // ─── 通信スレッド ──────────────────────────────────
    void ThreadMain();
    void InputThreadMain();
    void DrainAndProcessPackets();
    bool SendPacket(const std::vector<uint8_t> &data, int64_t dispatchTicks = 0);
    bool SendInputPackets(uint32_t head);
    void SendPendingInputs();
    void SleepUntil(int64_t targetUs);

    // ─── 状態 ──────────────────────────────────────────
    SharedSyncState _state;
    std::atomic<bool> _running{false};
    std::thread _thread, _inputThread;
    timer::ThreadSignal _networkWake, _inputWake;
    SyncMode _mode = SyncMode::WaitReady;

    // ─── 委譲先 ────────────────────────────────────────
    SyncCodec _calc;
    std::vector<uint8_t> _sendBuffer; // 通信スレッド専有。OS送信から戻ったら再利用可能。
    Metronome _metronome;

    // ─── 構成 ──────────────────────────────────────────
    bool _isHost = false;
    std::string _targetIp;
    uint16_t _targetPort = 0;
    uint16_t _localPort = 0;
    bool _startSent = false;

    // ─── 受信パケットキュー ─────────────────────────────
    std::mutex _recvMutex;
    std::vector<ReceivedPacket> _recvQueue;
    std::vector<ReceivedPacket> _recvQueueSwap;

    // ─── 入力バッファ writeHead 監視用 ─────────────────
    uint32_t _lastSentFrame = 0, _lastPeerAck = 0;
    uint32_t _lastLogFrame = 0;
    int _keepaliveCounter = 0;

    // ─── 実ピアポート（NAT越え用）──────────────────────
    uint16_t _peerActualPort = 0;
};

} // namespace netplay
} // namespace core
} // namespace cccaster
