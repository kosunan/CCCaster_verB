// ============================================================================
// SyncCodec.cpp — 同期計算器（実装）
//
// 【パケット設計】
//   全フェーズで SYNC_TICK (0x30) のみ使用。
//   flags.bit0=ready, bit1=phaseReady, startTimeTicks>0 で WaitReady/WaitStart を表現。

#include "shared_contracts/NetplaySettings.hpp"
#include "shared_contracts/SessionClosePacket.hpp"
#include "shared_contracts/PlayerName.hpp"
#include "core_dll/network/SyncCodec.hpp"
#include "core_dll/sync/NetplaySession.hpp"
#include "core_dll/timing/Metronome.hpp"
#include "core_dll/timing/WasapiClock.hpp"
#include "core_dll/sync/MatchInputBuffer.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/ui/State_Ui_Logic.hpp"
#include <cstring>
#include <algorithm>
#include "core_dll/sync/FrameSequence.hpp"
#include "core_dll/mbaa_mem/GamePhase.hpp"

namespace cccaster {
namespace core {
namespace netplay {

// ── 統一 SyncPayload ──
#pragma pack(push, 1)
struct SyncPayload {
    // NTP (常時)
    int64_t t_send;
    int64_t echo_t1;
    int64_t echo_t2;
    // フレーム同期 (Counting 時のみ有効)
    uint32_t baseFrame;
    uint8_t inputCount;
    // 接続時の設定照合値。次戦の変更は確定入力コマンドで同期する。
    uint8_t delay;
    uint8_t maxRollback;
    // フラグ (READY/PHASE_READY 統合)
    uint8_t flags; // bit0: ready, bit1: phaseReady
    // スタート時刻 (WaitStart 時のみ有効, 0=未設定)
    int64_t startTimeTicks;
    // 旧メニュー通知欄。版6では予約領域（選択は確定入力から計算）。
    int8_t retryMenuIndex;
    // Phase 遷移同期: InGame 開始時の writeHead 基準点 (0=未設定)
    uint32_t phaseBaseFrame;
    // 冗長入力 (最大10フレーム。0が最新baseFrame、1がbaseFrame-1...)
    uint32_t inputs[10];
    uint32_t seedEpoch;
    cccaster::game_interface::RngState seed;
    uint32_t consumedFrame;
    uint32_t appliedFrame;
    uint32_t nextCaptureFrame;
    int64_t nextCaptureTicks;
    sync::SelectionState selection;
    sync::EpochStart epochStart;
    sync::RetrySelection retry;
    char playerName[cccaster::public_api::PlayerNameSize];
    uint32_t clockGeneration; // WASAPIからQPCへの明示切替。
    uint32_t framePeriodTicks; // ホストが採用した論理1F。物理時計の補正とは別。
    uint32_t requestedPeriodTicks;
};
#pragma pack(pop)
static_assert(20 + sizeof(SyncPayload) + sizeof(cccaster::emblem::Chunk) <= 1200,
              "Emblem extension must fit a single conservative UDP datagram");
static_assert(20+sizeof(SyncPayload)+sizeof(cccaster::training_palette::network::Chunk)<=1200);

// ============================================================================
// BuildUnifiedPacket — CC10統一ヘッダ + ペイロードを組み立てる
// ============================================================================
void SyncCodec::BuildUnifiedPacket(std::vector<uint8_t> &pkt, uint8_t phase, uint8_t type, int64_t timestampTicks,
                                                   const void *payload, size_t payloadSize) {
    pkt.resize(UNIFIED_HEADER_SIZE + payloadSize);
    std::fill_n(pkt.data(), UNIFIED_HEADER_SIZE, uint8_t{0});
    uint32_t magic = CC10_MAGIC;
    std::memcpy(pkt.data(), &magic, sizeof(magic));
    pkt[4] = phase;
    pkt[5] = type;
    pkt[7] = 8; // wire 10拡張8: ステージ選択方法を共有。旧DLLとの混在を拒否。
    pkt[6] =
        cccaster::public_api::NetplaySettings::WireVersion; // 再戦の両者選択ゲート。旧DLLと混在させない。
    std::memcpy(pkt.data() + HDR_TIMESTAMP_OFFSET, &timestampTicks, sizeof(timestampTicks));
    if (payload && payloadSize > 0) {
        std::memcpy(pkt.data() + UNIFIED_HEADER_SIZE, payload, payloadSize);
    }
}

// ============================================================================
// Initialize / Reset
// ============================================================================
void SyncCodec::Initialize(bool isHost, int delayFrames, int maxRollback, Metronome *metronome) {
    _isHost = isHost;
    _delayFrames = delayFrames;
    _maxRollback = maxRollback;
    _metronome = metronome;
    Reset();
    _configurationLocked.store(true, std::memory_order_release);
}

void SyncCodec::Reset() {
    _emblems.Start(cccaster::emblem::Store::Get(0));
    _extraColors.Reset();_extraTurn=false;
    _peerClosed = false;
    _clock.Reset();
    _lastModelEvaluation = UINT32_MAX;
    _peerClockGeneration = UINT32_MAX;
    _clockValidAfterTicks = 0;
    _localClockGeneration = timer::WasapiClock::GetSourceGeneration();
    _peerReady = false;
    _framesSinceLastRecv = 0;
    _latestPeerFrame = 0;
    _peerScheduleSentTicks = 0;
    _peerProgressToken = 0;
    _peerAppliedFrame = 0;
    _peerProgressReceivedTicks = _peerProgressSentTicks = 0;
    _lastPeerT1 = 0;
    _lastPeerRecvTicks = 0;
    _delayDirty = false;
    _rollbackDirty = false;
}

// ============================================================================
// ProcessReceivedPacket — 受信 GAME_TICK 解析
// ============================================================================
void SyncCodec::ProcessReceivedPacket(const std::vector<uint8_t> &data, const std::string & /*fromIp*/,
                                      uint16_t /*fromPort*/, int64_t receiveTimeTicks) {
    // 統一ヘッダ検証
    if (static_cast<int>(data.size()) < UNIFIED_HEADER_SIZE)
        return;
    uint32_t magic = 0;
    std::memcpy(&magic, data.data(), sizeof(magic));
    if (magic != CC10_MAGIC)
        return;
    if (data[6] != cccaster::public_api::NetplaySettings::WireVersion) {
        NetplaySession::GetMutableState().protocolError.store(true, std::memory_order_release);
        return;
    }

    uint8_t pktType = data[5];
    if (cccaster::public_api::IsSessionClosePacket(data)) {
        _peerClosed = true;
        return;
    }
    if (_peerClosed) return; // 遅れて届いた通常入力で終了を取り消さない。
    if (pktType != PKT_SYNC_TICK)
        return;
    if (data[7] != 8) {
        NetplaySession::GetMutableState().protocolError = true;
        return;
    }
    if (data.size() < UNIFIED_HEADER_SIZE + sizeof(SyncPayload))
        return;

    SyncPayload gtp{};
    std::memcpy(&gtp, data.data() + UNIFIED_HEADER_SIZE, sizeof(gtp));

    // 時刻の差分・外挿を安全な範囲に限定する。0は未設定のまま許可。
    const auto validTime = [](int64_t t) { return t >= 0 && t <= INT64_MAX/4; };
    if (!validTime(receiveTimeTicks) || !validTime(gtp.t_send) ||
        !validTime(gtp.echo_t1) || !validTime(gtp.echo_t2) ||
        !validTime(gtp.nextCaptureTicks) || !validTime(gtp.startTimeTicks) ||
        !validTime(gtp.epochStart.hostTicks)) return;
    if (!timer::NetworkPacing::Valid(gtp.framePeriodTicks) ||
        !timer::NetworkPacing::Valid(gtp.requestedPeriodTicks)) return;

    // 不正な入力範囲を受理して疎通を延命したり、フレームを逆周回させない。
    if (gtp.inputCount > 10 || gtp.inputCount > gtp.baseFrame)
        return;
    _framesSinceLastRecv = 0;
    auto &shared = NetplaySession::GetMutableState();
    if (!gtp.selection.Valid()) return;
    if (!gtp.epochStart.Valid()) return;
    if (!gtp.retry.Valid()) return;
    {
        char receivedName[cccaster::public_api::PlayerNameSize + 1]{};
        std::memcpy(receivedName, gtp.playerName, sizeof(gtp.playerName));
        std::lock_guard lock(shared.playerNameMutex);
        cccaster::public_api::NormalizePlayerName(shared.peerPlayerName, receivedName, "PLAYER");
    }
    {
        std::lock_guard lock(shared.retryMutex);
        shared.peerRetry.Accept(gtp.retry);
    }
    {
        std::lock_guard lock(shared.selectionMutex);
        shared.peerSelection.Accept(gtp.selection);
    }
    if (gtp.consumedFrame > shared.peerConsumedFrame.load(std::memory_order_relaxed))
        shared.peerConsumedFrame.store(gtp.consumedFrame, std::memory_order_release);
    if (!_isHost && gtp.seedEpoch > 0 && gtp.seedEpoch == gtp.phaseBaseFrame) {
        std::lock_guard<std::mutex> lock(shared.seedMutex);
        if (gtp.seedEpoch > shared.peerSeedEpoch) {
            shared.peerSeed = gtp.seed;
            shared.peerSeedEpoch = gtp.seedEpoch;
        }
    }
    if (gtp.delay != _delayFrames || gtp.maxRollback != _maxRollback) {
        shared.protocolError.store(true, std::memory_order_release);
        return;
    }

    // ロード中の相手にも、イントロへ先着した相手にも同じ対象世代を通知する。
    if (gtp.flags & FLAG_PRESENT_ROLLBACK)
        shared.peerPresentRollback.store(true, std::memory_order_release);
    // 両ビット同時・境界外は無視。再送の順序が逆転しても受信世代を戻さない。
    const auto skipFlags = gtp.flags & (FLAG_LOADING_SKIP_CURRENT | FLAG_LOADING_SKIP_NEXT);
    if (gtp.phaseBaseFrame && gtp.phaseBaseFrame % sync::FrameSequence::STRIDE == 0 &&
        gtp.phaseBaseFrame <= 65534u * sync::FrameSequence::STRIDE &&
        (skipFlags == FLAG_LOADING_SKIP_CURRENT ||
         (skipFlags == FLAG_LOADING_SKIP_NEXT && gtp.phaseBaseFrame < 65534u * sync::FrameSequence::STRIDE))) {
        const uint32_t epoch = gtp.phaseBaseFrame +
            (skipFlags == FLAG_LOADING_SKIP_NEXT ? sync::FrameSequence::STRIDE : 0);
        if (epoch > shared.peerLoadingSkipEpoch.load(std::memory_order_relaxed))
            shared.peerLoadingSkipEpoch.store(epoch, std::memory_order_release);
    }

    if(data.size()==UNIFIED_HEADER_SIZE+sizeof(SyncPayload)+sizeof(cccaster::training_palette::network::Chunk)) {
        cccaster::training_palette::network::Chunk chunk;
        std::memcpy(&chunk,data.data()+UNIFIED_HEADER_SIZE+sizeof(SyncPayload),sizeof(chunk));
        _extraColors.Receive(chunk);
    }
    if (data.size() == UNIFIED_HEADER_SIZE + sizeof(SyncPayload) + sizeof(cccaster::emblem::Chunk)) {
        cccaster::emblem::Chunk chunk;
        std::memcpy(&chunk, data.data() + UNIFIED_HEADER_SIZE + sizeof(SyncPayload), sizeof(chunk));
        if (auto image = _emblems.Receive(chunk)) {
            cccaster::domain::session::DebugLog("[Emblem] peer received id=%08x", image->id);
            cccaster::emblem::Store::Set(1, std::move(image));
        }
    }

    const uint64_t progressToken = (uint64_t(gtp.phaseBaseFrame) << 32) | data[4];
    if ((gtp.flags & FLAG_PHASE_READY) && gtp.phaseBaseFrame > 0 && gtp.appliedFrame >= gtp.phaseBaseFrame &&
        gtp.appliedFrame - gtp.phaseBaseFrame < sync::FrameSequence::STRIDE &&
        (progressToken > _peerProgressToken ||
         (progressToken == _peerProgressToken && gtp.t_send > _peerProgressSentTicks &&
          gtp.appliedFrame >= _peerAppliedFrame))) {
        _peerProgressToken = progressToken;
        _peerAppliedFrame = gtp.appliedFrame;
        _peerProgressReceivedTicks = receiveTimeTicks;
        _peerProgressSentTicks = gtp.t_send;
    }

    // 時計ソース切替後は古い速度モデルを使わず学習をやり直す。
    const auto localGeneration = timer::WasapiClock::GetSourceGeneration();
    const bool currentSource = _peerClockGeneration == UINT32_MAX ||
                               gtp.clockGeneration >= _peerClockGeneration;
    if (localGeneration != _localClockGeneration ||
        (_peerClockGeneration != UINT32_MAX && gtp.clockGeneration > _peerClockGeneration)) {
        _clock.Reset();
        _clockValidAfterTicks = receiveTimeTicks;
        {
            std::lock_guard lock(shared.scheduleMutex);
            shared.peerSchedule = {};
        }
        _lastModelEvaluation = UINT32_MAX;
        domain::session::DebugLog("[PeerClock] reset reason=clock-source local=%u peer=%u",
                                  localGeneration,gtp.clockGeneration);
    }
    _localClockGeneration = localGeneration;
    if (currentSource) _peerClockGeneration = gtp.clockGeneration;
    if (currentSource && gtp.echo_t1 > 0 && gtp.echo_t1 >= _clockValidAfterTicks && gtp.echo_t2 > 0)
        _clock.AddNtpSampleTicks(gtp.echo_t1,gtp.echo_t2,gtp.t_send,receiveTimeTicks);
    const auto &model = _clock.Model();
    if (_lastModelEvaluation != model.Evaluations()) {
        _lastModelEvaluation = model.Evaluations();
        domain::session::DebugLog("[PeerClock] state=%s revision=%u localTicks=%lld ratePpb=%lld residualTicks=%lld uncertaintyTicks=%lld rateErrorPpb=%lld windowCount=%d windowUncertaintyTicks=%lld windowRateErrorPpb=%lld",
            timer::PeerClockModel::Name(model.Status(receiveTimeTicks)),model.Revision(),receiveTimeTicks,
            int64_t(std::llround(model.DriftRate(receiveTimeTicks)*1e9)),model.ResidualTicks(),
            model.UncertaintyTicks(),model.RateUncertaintyPpb(),model.WindowCount(),
            model.WindowUncertaintyTicks(),model.WindowRateErrorPpb());
    }

    if (currentSource && (gtp.flags & FLAG_PHASE_READY) && gtp.nextCaptureFrame > gtp.phaseBaseFrame &&
        gtp.nextCaptureFrame - gtp.phaseBaseFrame < sync::FrameSequence::STRIDE && gtp.nextCaptureTicks > 0 &&
        std::abs(gtp.nextCaptureTicks - gtp.t_send) < 2000000LL*60 && _clock.HasTimingEstimate()) {
        std::lock_guard lock(shared.scheduleMutex);
        if (gtp.phaseBaseFrame >= shared.peerSchedule.base && gtp.t_send > _peerScheduleSentTicks) {
            _peerScheduleSentTicks = gtp.t_send;
            shared.peerSchedule = {gtp.phaseBaseFrame, gtp.nextCaptureFrame,
                                   _clock.PeerToLocalTicks(gtp.nextCaptureTicks, receiveTimeTicks), receiveTimeTicks,
                                   _clock.GetThetaTicks(), _clock.GetRttTicks(),
                                   (int64_t(gtp.framePeriodTicks) - timer::ClockFrame) * timer::ClockParts +
                                       model.PeriodCorrectionParts(receiveTimeTicks) * gtp.framePeriodTicks / timer::ClockFrame,
                                   model.Ready(receiveTimeTicks),model.Revision(),
                                   gtp.framePeriodTicks,gtp.requestedPeriodTicks};
        }
    }

    {
        std::lock_guard lock(shared.epochStartMutex);
        if (gtp.epochStart.epoch == gtp.phaseBaseFrame &&
            gtp.epochStart.NewerThan(shared.peerEpochStart)) {
            shared.peerEpochStart = gtp.epochStart;
            shared.peerEpochStartLocalTicks = !_isHost && _clock.HasTimingEstimate()
                ? _clock.PeerToLocalTicks(gtp.epochStart.hostTicks, receiveTimeTicks) : 0;
        }
    }

    // (2) エコー追跡更新
    if (currentSource) {
        _lastPeerT1 = gtp.t_send;
        _lastPeerRecvTicks = receiveTimeTicks;
    }

    // (3) READY フラグ
    if (gtp.flags & FLAG_READY) {
        if (!_peerReady) {
            _peerReady = true;
            cccaster::domain::session::DebugLog("[SyncCodec] Peer READY received.");
        }
    }

    // (4) START 時刻
    if (gtp.startTimeTicks > 0) {
        _clock.SetPeerStartTime(gtp.startTimeTicks / 60);
        cccaster::domain::session::DebugLog("[SyncCodec] Peer startTimeTicks=%lld", gtp.startTimeTicks);
    }

    // (5) 該当バッファへの相手入力確定書込み（フレーム>0 なら Counting 中）

    if (gtp.baseFrame > 0 && gtp.inputCount > 0) {
        // パケットロス耐性向上のため、通信パケットは過去複数の入力(最大10)を保持している。
        // これを古いフレームから順に(または受信したすべてを)適用・確定する。
        for (int i = 0; i < gtp.inputCount; i++) {
            uint32_t historicFrame = gtp.baseFrame - i;
            if (historicFrame == 0)
                break;

            uint32_t remoteInput = gtp.inputs[i];
            cccaster::core::sync::MatchInputBuffer::GetInstance().ConfirmRemote(historicFrame, remoteInput);
        }
    } else if (gtp.baseFrame > 0) {
        cccaster::domain::session::DebugLog(
            "[SyncCodec] Ignoring packet: baseFrame=%u inputCount=%d flags=%x", gtp.baseFrame, gtp.inputCount,
            gtp.flags);
    }

    // D/Rはセッション中固定。途中変更を受信して消費位置を飛ばさない。

    // (7) 相手フレーム追跡
    if (gtp.baseFrame > _latestPeerFrame) {
        _latestPeerFrame = gtp.baseFrame;
    }

    // 世代は単調。遅延した古い通知で現在のバリアを解除しない。
    const uint32_t peerBase = shared.peerPhaseBaseFrame.load(std::memory_order_relaxed);
    if ((gtp.flags & FLAG_PHASE_READY) && gtp.phaseBaseFrame >= peerBase && gtp.phaseBaseFrame > 0) {
        if (gtp.phaseBaseFrame == peerBase && data[4] != shared.peerPhaseKind.load()) {
            shared.protocolError.store(true, std::memory_order_release);
        } else {
            shared.peerPhaseKind.store(data[4], std::memory_order_relaxed);
            shared.peerPhaseBaseFrame.store(gtp.phaseBaseFrame, std::memory_order_release);
            shared.peerPhaseReady.store(true, std::memory_order_release);
            shared.peerPhaseToken.store((uint64_t(gtp.phaseBaseFrame) << 32) | data[4],
                                        std::memory_order_release);
        }
    }
}

// ============================================================================
// BuildPacket — 全フェーズ共通パケット組立て
// ============================================================================
uint32_t SyncCodec::RepairWindowEnd(uint32_t frame) const {
    const auto &state = NetplaySession::GetState();
    const uint32_t base = uint32_t(state.localPhaseToken.load(std::memory_order_acquire) >> 32);
    if (!base || frame < base) return frame;
    const auto ack = state.peerConsumedFrame.load(std::memory_order_acquire);
    const auto anchor = ack >= base && ack - base < sync::FrameSequence::STRIDE ? ack : base;
    return std::min(frame, anchor + 10);
}
std::vector<uint8_t> SyncCodec::BuildPacket(uint32_t frame, uint32_t localInput, bool ready,
                                            int64_t startTimeUs, bool latestInputWindow) {
    std::vector<uint8_t> packet;
    BuildPacketInto(packet, frame, localInput, ready, startTimeUs, latestInputWindow);
    return packet;
}
void SyncCodec::BuildPacketInto(std::vector<uint8_t> &packet, uint32_t frame, uint32_t localInput, bool ready,
                                int64_t startTimeUs, bool latestInputWindow) {
    int64_t now = timer::WasapiClock::GetTimeTicks();
    SyncPayload gtp{};
    gtp.t_send = now;
    gtp.echo_t1 = _lastPeerT1;
    gtp.echo_t2 = _lastPeerRecvTicks;
    const auto &state = NetplaySession::GetState();
    {
        auto &shared = NetplaySession::GetMutableState();
        std::lock_guard lock(shared.playerNameMutex);
        std::memcpy(gtp.playerName, shared.localPlayerName, sizeof(gtp.playerName));
    }
    const uint64_t phaseToken = state.localPhaseToken.load(std::memory_order_acquire);
    const uint32_t base = uint32_t(phaseToken >> 32);
    {
        auto &shared = NetplaySession::GetMutableState();
        std::lock_guard lock(shared.selectionMutex);
        gtp.selection = shared.localSelection;
    }
    if (base && gtp.selection.epoch == base &&
        uint8_t(phaseToken) == static_cast<uint8_t>(game_interface::GamePhase::CharaSelect)) frame = 0;
    {
        auto &shared = NetplaySession::GetMutableState();
        std::lock_guard lock(shared.retryMutex);
        gtp.retry = shared.localRetry;
    }
    if (base && gtp.retry.epoch == base &&
        uint8_t(phaseToken) == static_cast<uint8_t>(game_interface::GamePhase::Rematch)) {
        // まだ旧戦闘にいる相手には末尾入力を保持・再送する。再戦入力は送らない。
        frame = state.peerPhaseBaseFrame.load() < base ? state.retryPreviousFrame.load() : 0;
    }
    // 独立入力時計が進んでも、相手の未消費先頭を冗長窓から落とさない。
    if (!latestInputWindow) frame = RepairWindowEnd(frame);
    {
        auto &shared = NetplaySession::GetMutableState();
        std::lock_guard lock(shared.epochStartMutex);
        gtp.epochStart = shared.localEpochStart;
    }
    gtp.baseFrame = frame;

    // 確定済みの連続履歴だけを送る。欠落は実入力0とは異なる。
    // localInput は互換性のため引数に残すが、keepaliveの0を確定値にしない。
    (void)localInput;
    gtp.inputCount = 0;
    const uint32_t maxCount = std::min<uint32_t>(frame, 10);
    for (uint32_t i = 0; i < maxCount; ++i) {
        uint32_t histInput = 0;
        if (!cccaster::core::sync::MatchInputBuffer::GetInstance().TryGetLocalInput(frame - i, histInput))
            break;
        gtp.inputs[i] = histInput;
        ++gtp.inputCount;
    }

    gtp.delay = static_cast<uint8_t>(_delayFrames);
    gtp.maxRollback = static_cast<uint8_t>(_maxRollback);

    gtp.flags = ready ? FLAG_READY : 0;
    if (state.localPresentRollback.load(std::memory_order_acquire)) gtp.flags |= FLAG_PRESENT_ROLLBACK;
    if (phaseToken != 0)
        gtp.flags |= FLAG_PHASE_READY;
    const auto skipEpoch = state.localLoadingSkipEpoch.load(std::memory_order_acquire);
    if (base && skipEpoch == base) gtp.flags |= FLAG_LOADING_SKIP_CURRENT;
    else if (base && base < 65534u * sync::FrameSequence::STRIDE &&
             skipEpoch == base + sync::FrameSequence::STRIDE) gtp.flags |= FLAG_LOADING_SKIP_NEXT;
    gtp.startTimeTicks = startTimeUs * 60;
    gtp.clockGeneration = timer::WasapiClock::GetSourceGeneration();

    gtp.retryMenuIndex = -1; // 予約領域

    gtp.phaseBaseFrame = uint32_t(phaseToken >> 32);
    gtp.appliedFrame = state.appliedFrame.load(std::memory_order_acquire);
    gtp.framePeriodTicks = gtp.requestedPeriodTicks = timer::NetworkPacing::Normal;
    {
        auto &shared = NetplaySession::GetMutableState();
        std::lock_guard lock(shared.scheduleMutex);
        if (shared.localSchedule.base == gtp.phaseBaseFrame) {
            gtp.nextCaptureFrame = shared.localSchedule.frame;
            gtp.nextCaptureTicks = shared.localSchedule.dueTicks;
            gtp.framePeriodTicks = shared.localSchedule.periodTicks;
            gtp.requestedPeriodTicks = shared.localSchedule.requestedPeriodTicks;
        }
    }

    gtp.consumedFrame = NetplaySession::GetState().consumedFrame.load(std::memory_order_acquire);
    if (_isHost) {
        auto &shared = NetplaySession::GetMutableState();
        std::lock_guard<std::mutex> lock(shared.seedMutex);
        gtp.seedEpoch = shared.localSeedEpoch;
        gtp.seed = shared.localSeed;
    }
    BuildUnifiedPacket(packet, uint8_t(phaseToken), PKT_SYNC_TICK, now, &gtp, sizeof(gtp));
    cccaster::emblem::Chunk chunk;
    _extraTurn=!_extraTurn;
    if (!_extraTurn && _emblems.Next(now / 60, chunk)) {
        const auto offset = packet.size();
        packet.resize(offset + sizeof(chunk));
        std::memcpy(packet.data() + offset, &chunk, sizeof(chunk));
    } else {
        cccaster::training_palette::network::Chunk color;
        if(_extraColors.Next(color,now/60)) {
            const auto offset=packet.size();packet.resize(offset+sizeof(color));
            std::memcpy(packet.data()+offset,&color,sizeof(color));
        }
    }
}

// ============================================================================
// UpdateAlphaCorrections — 採用時計の周期補正を Metronome に反映
// ============================================================================
void SyncCodec::UpdateAlphaCorrections() {
    if (!_metronome)
        return;

    // ホストの刻みを基準にする。両端が互いを追って振動することを避ける。
    _metronome->SetPeriodCorrectionParts(_isHost ? 0 : _clock.Model().PeriodCorrectionParts(timer::WasapiClock::GetTimeTicks()));
}

} // namespace netplay
} // namespace core
} // namespace cccaster
