#include "core_dll/sync/InputTimeline.hpp"
#include "shared_contracts/IpcData.hpp"
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif
// ============================================================================
// NetplaySession.cpp — 通信スレッド統括
//
// 【設計】
//   通信スレッドは送受信、専用時計スレッドはWASAPI入力採取を担当する。
//   パケット解析・Θ計算・α補正・FrameInputBuffer操作は SyncCodec に委譲。
//   フレームリズム生成は Metronome に委譲。
// ============================================================================

#include "core_dll/sync/NetplaySession.hpp"
#include "core_dll/common/TimeScale.hpp"
#include "core_dll/timing/WasapiClock.hpp"
#include "core_dll/network/NetplayManager.hpp"
#include "core_dll/sync/MatchInputBuffer.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/ui/State_Ui_Logic.hpp"
#include "core_dll/common/Platform.hpp"
#include "core_dll/network/NetworkSimulator.hpp"
#include "core_dll/mbaa_mem/IGameMemory.hpp"

namespace cccaster {
namespace core {
namespace netplay {

namespace {
bool InputSendBaseline() {
    static const bool baseline = std::getenv("CCCASTER_INPUT_SEND_BASELINE") != nullptr;
    return baseline;
}
bool InputSendTrace() {
    static const bool trace = std::getenv("CCCASTER_INPUT_SEND_TRACE") != nullptr;
    return trace;
}
// 明示的な診断時だけ採取。CPU時間はスリープ中の経過を含めない。
struct WorkerStats {
    bool enabled = std::getenv("CCCASTER_FRAME_TIMING_TRACE") != nullptr;
    int64_t wall = 0, cpu = 0, maxWork = 0;
    unsigned iterations = 0;
    static int64_t CpuUs() {
#ifdef _WIN32
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
            const uint64_t k = (uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
            const uint64_t u = (uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime;
            return static_cast<int64_t>((k + u) / 10);
        }
#endif
        return 0;
    }
    void Tick(const char *role, int64_t work = 0) {
        if (!enabled)
            return;
        const auto now = platform::RealMonotonicUs();
        if (!wall) {
            wall = now;
            cpu = CpuUs();
        }
        ++iterations;
        maxWork = std::max(maxWork, work);
        if (now - wall < 2000000)
            return;
        const auto used = CpuUs();
        domain::session::DebugLog(
            "[ThreadCost] role=%s windowUs=%lld cpuUs=%lld iterations=%u maxWorkUs=%lld", role, now - wall,
            used - cpu, iterations, maxWork);
        wall = now;
        cpu = used;
        iterations = 0;
        maxWork = 0;
    }
};
} // namespace

// ─── シングルトン ──────────────────────────────────────
NetplaySession &NetplaySession::GetInstance() {
    // Session停止時まで入力時計のmutexを生存させる。
    cccaster::core::sync::InputTimeline::GetInstance();
    // 専用スレッドを止める前に時計・時計mutexが静的破棄されないよう先に構築する。
    static const bool clockInitialized = [] {
        timer::WasapiClock::GetTimeUs();
        return true;
    }();
    (void)clockInitialized;
    static NetplaySession instance;
    return instance;
}

// ============================================================================
// Start — 通信スレッド + メトロノームを起動
// ============================================================================
void NetplaySession::Start(bool isHost, const std::string &targetIp, uint16_t targetPort, uint16_t localPort,
                           int delayFrames, int maxRollback, const char *playerName) {
    if (_running.load())
        return;

    cccaster::core::sync::InputTimeline::GetInstance().Reset();
    _isHost = isHost;
    _targetIp = targetIp;
    _targetPort = targetPort;
    _localPort = localPort;
    _startSent = false;
    _lastSentFrame = _lastPeerAck = 0;
    _state.appliedFrame.store(0);
    {
        std::lock_guard lock(_state.epochStartMutex);
        _state.localEpochStart = _state.peerEpochStart = {};
        _state.peerEpochStartLocalTicks = 0;
    }
    {
        std::lock_guard lock(_state.selectionMutex);
        _state.localSelection = {};
        _state.peerSelection = {};
    }
    {
        std::lock_guard lock(_state.retryMutex);
        _state.localRetry = {};
        _state.peerRetry = {};
        _state.retryPreviousFrame = 0;
    }
    {
        std::lock_guard lock(_state.scheduleMutex);
        _state.localSchedule = {};
        _state.peerSchedule = {};
    }
    _lastLogFrame = 0;
    _peerActualPort = 0;
    {
        std::lock_guard lock(_state.playerNameMutex);
        cccaster::public_api::NormalizePlayerName(_state.localPlayerName, playerName,
                                                   isHost ? "PLAYER 1" : "PLAYER 2");
        _state.peerPlayerName[0] = '\0';
    }

    // SharedSyncState リセット
    _state.localPhaseToken.store(0);
    _state.consumedFrame.store(0);
    _state.peerConsumedFrame.store(0);
    {
        std::lock_guard<std::mutex> lock(_state.seedMutex);
        _state.localSeedEpoch = 0;
        _state.peerSeedEpoch = 0;
    }
    _state.peerPhaseToken.store(0);
    _state.phaseBaseFrame.store(0);
    _state.peerPhaseBaseFrame.store(0);
    _state.localPhaseKind.store(0);
    _state.peerPhaseKind.store(0);
    _state.localPhaseReady.store(false);
    _state.peerPhaseReady.store(false);
    _state.protocolError.store(false);
    _state.needKeepalive.store(true);
    _keepaliveCounter = 0;
    _state.currentTickUs.store(Metronome::BASE_TICK_US);
    _state.isSynced.store(false);
    _state.isPeerAlive.store(false);
    _state.peerReady.store(false);
    _state.clockOffsetUs.store(0);
    _state.lastRttUs.store(0);

    // InputBuffer 初期化
    cccaster::core::sync::MatchInputBuffer::GetInstance().Initialize(200, delayFrames, maxRollback);

    // オーバーレイ初期表示
    cccaster::domain::ui::StateUiLogic::SetDelay(delayFrames);
    cccaster::domain::ui::StateUiLogic::SetRollback(maxRollback);

    // SyncCodec 初期化
    _calc.Initialize(isHost, delayFrames, maxRollback, &_metronome);
    _sendBuffer.reserve(512);

    // キュークリア
    {
        std::lock_guard<std::mutex> lock(_recvMutex);
        _recvQueue.clear();
        _recvQueueSwap.clear();
    }

    // モード初期化
    _mode = SyncMode::WaitReady;

    cccaster::domain::session::DebugLog(
        "[NetplaySession] Starting. host=%d target=%s:%u localPort=%u delay=%d maxRB=%d", isHost,
        targetIp.c_str(), targetPort, localPort, delayFrames, maxRollback);

    _running.store(true);
    try {
        _inputThread = std::thread(&NetplaySession::InputThreadMain, this);
        _thread = std::thread(&NetplaySession::ThreadMain, this);
    } catch (...) {
        Stop();
        throw;
    }
}

// ============================================================================
// Stop — 通信スレッド + メトロノームを停止
// ============================================================================
void NetplaySession::Stop() {
    if (!_running.load())
        return;
    _running.store(false);
    _inputWake.Notify();
    _networkWake.Notify();
    if (_inputThread.joinable())
        _inputThread.join();
    cccaster::core::sync::InputTimeline::GetInstance().Pause();
    if (_thread.joinable()) {
        _thread.join();
    }
    _metronome.Stop();
    cccaster::domain::session::DebugLog("[NetplaySession] Stopped.");
}

// ============================================================================
// OnPacketReceived — 受信スレッドから呼ばれ、キューに Push するだけ
// ============================================================================
void NetplaySession::OnPacketReceived(const std::vector<uint8_t> &data, const std::string &fromIp,
                                      uint16_t fromPort) {
    int64_t receiveTime = timer::WasapiClock::GetTimeTicks();
    std::lock_guard<std::mutex> lock(_recvMutex);
    _recvQueue.push_back({data, fromIp, fromPort, receiveTime});
    _networkWake.Notify();
}

// ============================================================================
// DrainAndProcessPackets — 受信キューを drain して SyncCodec に委譲
// ============================================================================
void NetplaySession::DrainAndProcessPackets() {
    {
        std::lock_guard<std::mutex> lock(_recvMutex);
        std::swap(_recvQueue, _recvQueueSwap);
    }

    for (const auto &pkt : _recvQueueSwap) {
        // 疎通更新
        // 検証済みパケットだけが疎通を更新する。
        _peerActualPort = pkt.fromPort;

        // SyncCodec に処理を委譲
        if (pkt.fromPort != _targetPort || pkt.fromIp != _targetIp)
            continue;
        static const bool stageTrace = std::getenv("CCCASTER_PACE_TRACE") != nullptr;
        const auto queueAge = stageTrace ? (timer::WasapiClock::GetTimeTicks() - pkt.receiveTimeTicks)/60 : 0;
        const auto decodeStart = stageTrace ? platform::RealMonotonicUs() : 0;
        const auto previousRttSample = _calc.GetRttSampleSerial();
        _calc.ProcessReceivedPacket(pkt.data, pkt.fromIp, pkt.fromPort, pkt.receiveTimeTicks);
        if (_calc.GetRttSampleSerial() != previousRttSample)
            domain::ui::StateUiLogic::RecordNetworkSample(_calc.GetLatestRttUs() / 1000.0f);
        if (_calc.IsPeerClosed()) {
            cccaster::domain::session::DebugLog("[SessionClose] Peer closed the game. Exiting immediately.");
            cccaster::public_api::IpcManager::UpdateOrReadState([](cccaster::public_api::SharedState &s) {
                s.lastErrorCode = static_cast<uint32_t>(cccaster::public_api::SessionErrorType::PeerClosed);
                s.syncCompleted = false;
            });
            // ゲームスレッドがロード・入力待機中でも終了できる。ゲームメモリには触れない。
            platform::TerminateSelf();
        }
        if (stageTrace && (queueAge > 100 || platform::RealMonotonicUs() - decodeStart > 100))
            domain::session::DebugLog("[ReceiveStage] queue=%lld decode=%lld", queueAge,
                                      platform::RealMonotonicUs() - decodeStart);
        _state.isPeerAlive.store(_calc.IsPeerAlive(), std::memory_order_release);

        // SharedSyncState 更新
        _state.clockOffsetUs.store(_calc.GetThetaUs(), std::memory_order_release);
        _state.lastRttUs.store(_calc.GetRttUs(), std::memory_order_release);
        if (_calc.IsPeerReady()) {
            _state.peerReady.store(true, std::memory_order_release);
        }
    }

    _recvQueueSwap.clear();
}

// ============================================================================
// SendPacket — NetplayManager 経由で送信
// ============================================================================
bool NetplaySession::SendPacket(const std::vector<uint8_t> &data, int64_t dispatchTicks) {
    auto &manager = cccaster::netplay::NetplayManager::GetInstance();
    auto *socket = manager.GetUdpSocket();
    if (!socket) return false;
    cccaster::network::SendProbe probe;
    // wire 10: ヘッダ20B + NTP24Bの後に実際の送信先頭F。再送窓を最新Fと誤記しない。
    if (InputSendTrace() && dispatchTicks && data.size() > 48 && data[48]) {
        std::memcpy(&probe.frame, data.data() + 44, sizeof(probe.frame));
        probe.dispatch = dispatchTicks;
        probe.prepared = platform::RealMonotonicTicks();
        probe.clock = platform::RealMonotonicTicks;
        probe.report = [](const cccaster::network::SendProbe &p, int64_t begin, int64_t end,
                           bool success, bool direct) {
            domain::session::DebugLog(
                "[InputSend] f=%u dispatch=%lld prepared=%lld submit=%lld complete=%lld ok=%d direct=%d",
                p.frame, p.dispatch, p.prepared, begin, end, success, direct);
        };
    }
    if (InputSendBaseline()) {
        socket->Send(manager.GetTargetIp(), manager.GetTargetPort(), data, probe);
        return true;
    }
    return socket->SendPeer(data, probe);
}

// ============================================================================
// SleepUntil — 精密スリープ
// ============================================================================
bool NetplaySession::SendInputPackets(uint32_t head) {
    // 未消費先頭の再送を残し、高RTTでも最新入力をACK待ちで止めない。
    const auto dispatch = InputSendTrace() ? platform::RealMonotonicTicks() : 0;
    if (InputSendBaseline()) {
        SendPacket(_calc.BuildPacket(head, 0), dispatch);
        if (_calc.RepairWindowEnd(head) != head)
            SendPacket(_calc.BuildPacket(head, 0, false, 0, true), dispatch);
        return true;
    }
    _calc.BuildPacketInto(_sendBuffer, head, 0, false, 0, true);
    const bool latestSent = SendPacket(_sendBuffer, dispatch);
    bool repairSent = true;
    if (_calc.RepairWindowEnd(head) != head) {
        _calc.BuildPacketInto(_sendBuffer, head, 0);
        repairSent = SendPacket(_sendBuffer, dispatch);
    }
    if (!latestSent || !repairSent) {
        // 非ブロッキング失敗を成功と誤認しない。次の採取／ACK／定期送信で履歴を再送。
        domain::session::DebugLog("[InputSendFailure] f=%u latest=%d repair=%d", head, latestSent, repairSent);
    }
    return latestSent && repairSent;
}

void NetplaySession::SendPendingInputs() {
    if (_mode != SyncMode::Counting) return;
    const auto head = cccaster::core::sync::MatchInputBuffer::GetInstance().GetWriteHead();
    const auto ack = _state.peerConsumedFrame.load(std::memory_order_acquire);
    if (head != _lastSentFrame || ack != _lastPeerAck) {
        if (SendInputPackets(head)) {
            _lastPeerAck = ack;
            _lastSentFrame = head;
            _keepaliveCounter = 0;
        }
    }
}

void NetplaySession::SleepUntil(int64_t targetUs) {
    static thread_local WorkerStats stats;
    while (_running.load(std::memory_order_acquire)) {
        stats.Tick("network");
        const auto ticket = _networkWake.Ticket();
        // 新入力・受信の通知で起床。codecはこのスレッドだけが触る。
        if (!InputSendBaseline()) SendPendingInputs(); // 受信一括解析より先に新入力を出す。
        DrainAndProcessPackets();
        SendPendingInputs();
        const auto remaining = targetUs - timer::WasapiClock::GetTimeUs();
        if (remaining <= 0)
            break;
        _networkWake.Wait(ticket, remaining);
    }
}

// メトロノーム専用。通信解析・パケット生成・ゲーム状態の保存／復元を行わない。
void NetplaySession::InputThreadMain() {
    platform::TimingThread priority("input");
    auto &timeline = cccaster::core::sync::InputTimeline::GetInstance();
    cccaster::domain::session::DebugLog("[InputThread] Started (dedicated WASAPI capture) highResWait=%d",
                                        _inputWake.IsHighResolution());
    WorkerStats stats;
    for (;;) {
        const auto ticket = _inputWake.Ticket();
        if (!_running.load(std::memory_order_acquire))
            break;
        stats.Tick("input");
        auto due = timeline.NextDeadlineTicks();
        if (!due) {
            // ロード・合流中は無期限に眠る。Begin/Resume/Stopが起こす。
            _inputWake.Wait(ticket, -1);
            continue;
        }
        auto remaining = (due - timer::WasapiClock::GetTimeTicks()) / 60;
        constexpr int64_t spinMarginUs = 1000; // 実対戦の起床超過430us+旧500usから校正。
        if (remaining > spinMarginUs) {
            _inputWake.Wait(ticket, remaining - spinMarginUs);
            continue;
        }
        static const bool stageTrace = std::getenv("CCCASTER_PACE_TRACE") != nullptr;
        const auto spinBegin = stageTrace ? platform::RealMonotonicUs() : 0;
        // 最後だけ短時間スピン。通信キューや入力バッファを繰り返し走査しない。
        while (_running.load(std::memory_order_acquire) && timeline.IsActive() &&
               timer::WasapiClock::GetTimeTicks() < due)
            cccaster::platform::CpuRelax();
        if (!_running.load(std::memory_order_acquire))
            break;
        const auto spinEnd = stageTrace ? platform::RealMonotonicUs() : 0;
        const auto started = stats.enabled ? platform::RealMonotonicUs() : 0;
        timeline.PumpTicks(timer::WasapiClock::GetTimeTicks(), _metronome.GetPeriodCorrectionParts());
        if (stageTrace)
            domain::session::DebugLog("[InputSpin] f=%u begin=%lld end=%lld", timeline.SampledFrame(),
                                      spinBegin, spinEnd);
        if (stats.enabled)
            stats.maxWork = std::max(stats.maxWork, platform::RealMonotonicUs() - started);
    }
    cccaster::domain::session::DebugLog("[InputThread] Exiting");
}

// ============================================================================
// ThreadMain — 通信スレッドのメインループ
// ============================================================================
//
// 【設計】
//   通信スレッドはパケット送受信に専念。
//   1フレーム間隔（BASE_TICK_US ≈ 16.6ms）でループ。
//   待機中は受信・新入力通知で起床して送受信を進める。
//   α補正はメトロノームが独立管理し、通信間隔に影響しない。
//
//   Counting モードでは以下2つのソースを監視:
//     (1) CB writeHead 変化 → ゲームデータ送信
//     (2) needKeepalive フラグ → 定期 keepalive 送信
//
void NetplaySession::ThreadMain() {
    cccaster::domain::session::DebugLog("[NetplaySession] Thread started. Mode=WaitReady");

    int64_t nextTickUs = timer::WasapiClock::GetTimeUs();

    while (_running.load()) {
        // ── 定期締切または受信・新入力通知まで待機 ──
        SleepUntil(nextTickUs);
        if (!_running.load(std::memory_order_acquire))
            break;
        int64_t now = timer::WasapiClock::GetTimeUs();

        // ── 受信パケット処理 ──
        DrainAndProcessPackets();

        switch (_mode) {
        // ================================================================
        // Mode::WaitReady — 準備完了待機
        // ================================================================
        case SyncMode::WaitReady: {
            SendPacket(_calc.BuildPacket(0, 0, true, 0));

            if (_calc.IsPeerReady()) {
                _mode = SyncMode::WaitStart;
                cccaster::domain::session::DebugLog(
                    "[NetplaySession] Mode -> WaitStart (peer READY received)");
            }
            break;
        }

        // ================================================================
        // Mode::WaitStart — 開始時刻待機
        // ================================================================
        case SyncMode::WaitStart: {
            // 時計はペース推定用。揺れの収束を必須にすると実回線で開始できない。
            // ゲームの開始と進行の一致は世代バリア+確定入力で保証する。
            SendPacket(_calc.BuildPacket(0, 0, true, 0));
            if (_calc.HasTimingEstimate()) {
                _mode = SyncMode::Counting;
                _calc.SetBaselineTheta();
                const uint32_t startFrame =
                    cccaster::core::sync::MatchInputBuffer::GetInstance().GetWriteHead();
                const uint32_t gameWorldTimer = cccaster::game_interface::GameMem().WorldTimer();
                _metronome.Start();
                _state.isPeerAlive.store(true, std::memory_order_release);
                _state.isSynced.store(true, std::memory_order_release);
                cccaster::domain::session::DebugLog(
                    "[NetplaySession] Mode -> Counting. startFrame=%u WT=%u (epoch gate owns progress)",
                    startFrame, gameWorldTimer);
            }
            break;
        }

        // ================================================================
        // Mode::Counting — CB writeHead + needKeepalive 監視
        // ================================================================
        case SyncMode::Counting: {
            // 疎通カウンタ + α補正
            _calc.IncrementFrameCount();
            _state.isPeerAlive.store(_calc.IsPeerAlive(), std::memory_order_release);
            _state.currentTickUs.store(_metronome.GetCurrentIntervalUs(), std::memory_order_release);
            _calc.UpdateAlphaCorrections();

            // (1) 入力バッファの writeHead 監視 → ゲームデータ送信
            //   フレーム空間はセッション通しで1本。フェーズでリセットしないため、
            //   writeHead が後退することはない。
            uint32_t newHead = cccaster::core::sync::MatchInputBuffer::GetInstance().GetWriteHead();

            bool sent = false;
            if (newHead > _lastSentFrame && SendInputPackets(newHead)) {
                _lastSentFrame = newHead;
                _keepaliveCounter = 0;
                sent = true;
            }

            if (!sent && _state.needKeepalive.load(std::memory_order_acquire)) {
                // (2) 入力書込みなし + keepalive要求 → 定期 keepalive
                _keepaliveCounter++;
                if (_keepaliveCounter >= KEEPALIVE_INTERVAL_FRAMES) {
                    if (SendInputPackets(newHead)) _keepaliveCounter = 0;
                }
            }

            // 進捗ログ（60フレームごと）
            if (newHead % 60 == 0 && newHead != _lastLogFrame) {
                cccaster::domain::session::DebugLog(
                    "[NetplaySession] wh=%u tick=%lldus correctionParts=%lld RTT=%lldus peerF=%u", newHead,
                    _metronome.GetCurrentIntervalUs(), _metronome.GetPeriodCorrectionParts(),
                    _calc.GetRttUs(), _calc.GetLatestPeerFrame());
                _lastLogFrame = newHead;
                auto &sim = cccaster::network::NetworkSimulator::Instance();
                if (sim.IsEnabled())
                    cccaster::domain::session::DebugLog(
                        "[NetworkTest] delayed=%llu dropped=%llu requestedUs=%llu observedUs=%llu",
                        sim.DelayedCount(), sim.DroppedCount(), sim.RequestedUs(), sim.ObservedUs());
            }
            break;
        }
        } // switch

        nextTickUs += cccaster::testing::ScaleTickUs(Metronome::BASE_TICK_US);
        if (nextTickUs < now)
            nextTickUs = now + cccaster::testing::ScaleTickUs(Metronome::BASE_TICK_US);
    }

    cccaster::domain::session::DebugLog("[NetplaySession] Thread exiting.");
}

} // namespace netplay
} // namespace core
} // namespace cccaster
