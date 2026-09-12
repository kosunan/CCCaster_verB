// ============================================================================
// WasapiClock.cpp — WASAPI IAudioClock ベースの高精度クロック（実装）
//
// 【初期化フロー】
//   1. COM 初期化 (COINIT_MULTITHREADED)
//   2. デフォルトオーディオ出力デバイスを取得
//   3. IAudioClient を共有モードで初期化（バッファ 1秒）
//   4. IAudioClock を取得し、frequency をキャッシュ
//   5. AudioClient を Start() し、クロックカウントの進行を開始
//
// 【フォールバック】
//   GetTimeUs() は WASAPI 優先、取得不可時は QPC にフォールバックする。
//   TimeHooks にフックされた QPC ではなく、RealQueryPerformanceCounter を
//   使用して実時間を取得する。
// ============================================================================

// 【Linux ビルドについて】
//   WASAPI は Windows のオーディオ API なので、Linux では初期化そのものを行わず
//   `_available = false` のまま単調時計にフォールバックする。フォールバック経路は
//   もともと「オーディオデバイスが無いヘッドレス環境」のために用意されていたもので、
//   Linux 用に新設したものではない。
//   ドリフト特性は Windows(WASAPI) と Linux(CLOCK_MONOTONIC) で当然異なるため、
//   **Linux の harness はクロック品質の検証には使えない**。同期ロジックの
//   決定性検証にのみ使うこと。

#include "core_dll/timing/WasapiClock.hpp"
#include "core_dll/common/Platform.hpp"

#include "core_dll/timing/ClockContinuity.hpp"
#include "core_dll/common/DebugLog.hpp"
#include <algorithm>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#endif

namespace cccaster {
namespace core {
namespace timer {

#ifdef _WIN32

// GUIDはSDKヘッダーの定義を使用し、手書きによる識別子の誤りを防ぐ。
// ─── コンストラクタ: WASAPI 初期化 ─────────────────────
void WasapiClock::InitializeAudio() {
    comInitialized_ = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    if (!comInitialized_)
        return;

    IMMDeviceEnumerator *pEnumerator = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator,
                                  (void **)&pEnumerator);
    if (FAILED(hr))
        return;

    IMMDevice *pDevice = nullptr;
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    if (FAILED(hr)) {
        pEnumerator->Release();
        return;
    }

    IAudioClient *pClient = nullptr;
    hr = pDevice->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&pClient);
    if (FAILED(hr)) {
        pDevice->Release();
        pEnumerator->Release();
        return;
    }

    WAVEFORMATEX *pwfx = nullptr;
    hr = pClient->GetMixFormat(&pwfx);
    if (FAILED(hr)) {
        pClient->Release();
        pDevice->Release();
        pEnumerator->Release();
        return;
    }

    // 共有モードで初期化。バッファ = 1秒 (10,000,000 × 100ns)。
    hr = pClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, pwfx, NULL);
    if (FAILED(hr)) {
        CoTaskMemFree(pwfx);
        pClient->Release();
        pDevice->Release();
        pEnumerator->Release();
        return;
    }

    // 再生位置を時計に使うため、空のストリームをStartするだけで済ませない。
    IAudioRenderClient *render = nullptr;
    if (SUCCEEDED(pClient->GetService(IID_IAudioRenderClient, (void **)&render))) {
        _pRenderClient = render;
        pClient->GetBufferSize(&_bufferFrames);
    }
    _pAudioClient = pClient;
    const bool primed = FillSilence();
    IAudioClock *pClock = nullptr;
    hr = pClient->GetService(IID_IAudioClock, (void **)&pClock);
    if (SUCCEEDED(hr) && pClock) {
        hr = pClock->GetFrequency(&_frequency);
        if (SUCCEEDED(hr) && _frequency > 0 && primed && SUCCEEDED(pClient->Start())) {
            _isStarted = true;
            _available = true;
            LARGE_INTEGER qpcFrequency{};
            QueryPerformanceFrequency(&qpcFrequency);
            domain::session::DebugLog(
                "[ClockResolution] qpcHz=%lld audioHz=%llu tickHz=60000000 frameTicks=1000000",
                qpcFrequency.QuadPart, _frequency);
        }
    }

    _pAudioClient = pClient;
    _pAudioClock = pClock;

    CoTaskMemFree(pwfx);
    pDevice->Release();
    pEnumerator->Release();
}

// ─── デストラクタ ──────────────────────────────────────
void WasapiClock::ReleaseAudio() {
    auto *pClient = static_cast<IAudioClient *>(_pAudioClient);
    auto *pClock = static_cast<IAudioClock *>(_pAudioClock);
    if (_isStarted && pClient)
        pClient->Stop();
    if (_pRenderClient)
        static_cast<IAudioRenderClient *>(_pRenderClient)->Release();
    if (pClock)
        pClock->Release();
    if (pClient)
        pClient->Release();
    if (comInitialized_)
        CoUninitialize();
}

// ─── WASAPI 時刻取得 ──────────────────────────────────
bool WasapiClock::FillSilence() {
    auto *client = static_cast<IAudioClient *>(_pAudioClient);
    auto *render = static_cast<IAudioRenderClient *>(_pRenderClient);
    if (!client || !render || !_bufferFrames)
        return false;
    UINT32 padding = 0;
    if (FAILED(client->GetCurrentPadding(&padding)) || padding > _bufferFrames)
        return false;
    const auto available = _bufferFrames - padding;
    if (!available)
        return true;
    BYTE *data = nullptr;
    if (FAILED(render->GetBuffer(available, &data)))
        return false;
    return SUCCEEDED(render->ReleaseBuffer(available, AUDCLNT_BUFFERFLAGS_SILENT));
}

int64_t WasapiClock::GetWasapiTimeTicks() {
    auto *pClock = static_cast<IAudioClock *>(_pAudioClock);
    if (!pClock)
        return 0;

    const auto now = GetQpcTimeUs();
    if (now - _lastFillUs >= 10000) {
        _lastFillUs = now;
        if (!FillSilence())
            return 0;
    }
    uint64_t pos = 0;
    uint64_t qpc = 0;
    HRESULT hr = pClock->GetPosition(&pos, &qpc);
    if (hr != S_OK || _frequency == 0 || pos == 0)
        return 0;

    // GetPositionのQPCは100ns単位。相関した計測点から現在までを補間する。
    const int64_t age = platform::RealMonotonicTicks() - static_cast<int64_t>(qpc * 6);
    if (!qpc || age < 0 || age > 100000 * 60) {
        static int diagnostics = 0;
        if (std::getenv("CCCASTER_FRAME_TIMING_TRACE") && diagnostics++ < 5)
            cccaster::domain::session::DebugLog("[AudioSample] pos=%llu freq=%llu ageUs=%lld hr=%ld", pos,
                                                _frequency, age / 60, hr);
        return 0;
    }
    return static_cast<int64_t>(pos / _frequency * 60000000 + pos % _frequency * 60000000 / _frequency) + age;
}

#else // !_WIN32 ────────────────────────────────────────────

// Linux: WASAPI は存在しない。_available は false のまま、
// GetTimeUs() は必ずフォールバック（単調時計）を通る。
void WasapiClock::InitializeAudio() {}
void WasapiClock::ReleaseAudio() {}
int64_t WasapiClock::GetWasapiTimeTicks() {
    return 0;
}

#endif // _WIN32

// ─── シングルトン ──────────────────────────────────────
WasapiClock &WasapiClock::GetInstance() {
    static WasapiClock instance;
    return instance;
}

// ─── フォールバック: 実時間の単調時計 ─────────────────
// Windows ではフック前の QPC、Linux では CLOCK_MONOTONIC。
// どちらも Platform 側に閉じ込めてある。
int64_t WasapiClock::GetQpcTimeUs() {
    return cccaster::platform::RealMonotonicUs();
}

// ─── 統合 API: WASAPI 優先、単調時計フォールバック ────
WasapiClock::WasapiClock() {
    const auto now = platform::RealMonotonicTicks();
    projection_.Publish({now, now, 0, 0});
    worker_ = std::thread([this] { Worker(); });
}
WasapiClock::~WasapiClock() {
    stopping_.store(true);
    wake_.Notify();
    if (worker_.joinable())
        worker_.join();
}
void WasapiClock::Worker() {
    ClockContinuity continuity(60);
    continuity.Read(platform::RealMonotonicTicks(), 0);
    InitializeAudio();
    fallback_.store(!_available.load(), std::memory_order_relaxed);
    if (!_available)
        domain::session::DebugLog("[Clock] WASAPI unavailable at startup; QPC fallback");
    bool reportedAudio = false;
    while (!stopping_.load()) {
        const auto ticket = wake_.Ticket();
        const auto started = GetQpcTimeUs();
        const bool wasFallback = continuity.IsFallback();
        const auto audio = _available && !wasFallback ? GetWasapiTimeTicks() : 0;
        continuity.Read(platform::RealMonotonicTicks(), audio);
        projection_.Publish(continuity.Anchor());
        if (audio > 0 && !continuity.IsFallback() && !reportedAudio) {
            reportedAudio = true;
            domain::session::DebugLog("[Clock] WASAPI active (silence-fed worker, lock-free readers)");
        }
        if (!wasFallback && continuity.IsFallback()) {
            fallback_.store(true, std::memory_order_relaxed);
            _available = false;
            domain::session::DebugLog("[Clock] WASAPI failed; continuous QPC fallback");
        }
        static const bool trace = std::getenv("CCCASTER_PACE_TRACE") != nullptr;
        const auto elapsed = GetQpcTimeUs() - started;
        if (trace && elapsed > 100)
            domain::session::DebugLog("[AudioWork] elapsed=%lld", elapsed);
        if (!stopping_.load())
            wake_.Wait(ticket, 10000);
    }
    ReleaseAudio();
}
uint32_t WasapiClock::GetSourceGeneration() { return GetInstance().IsFallback() ? 1u : 0u; }
int64_t WasapiClock::GetTimeUs() {
    return GetTimeTicks() / 60;
}
int64_t WasapiClock::GetTimeTicks() {
    return GetTimeTicks(nullptr);
}
int64_t WasapiClock::WaitForRelease(int64_t deadlineTicks) {
    ClockAnchor anchor;
    GetInstance().projection_.Read(anchor);
    auto now = platform::RealMonotonicTicks();
    const auto due = anchor.DeadlineTicks(deadlineTicks, now);
    releaseSample = {now, due, 0, anchor.AtTicks(now) - deadlineTicks, anchor.ppm};
    while (now < due) {
#if defined(__i386__) || defined(__x86_64__)
        __builtin_ia32_pause(); // 最終ループはcall/retを挟まずPAUSE命令1個。
#else
        platform::CpuRelax();
#endif
        now = platform::RealMonotonicTicks();
    }
    releaseSample.exit = now;
    return now;
}
int64_t WasapiClock::GetTimeTicks(ReadSample *sample) {
    auto &inst = GetInstance();
    thread_local ClockAnchor cached;
    thread_local int64_t previous = 0;
    const auto old = sample ? cached : ClockAnchor{};
    inst.projection_.Read(cached);
    const auto qpc = platform::RealMonotonicTicks();
    const auto candidate = cached.AtTicks(qpc);
    // 読取り競合で古いモデルを使っても、同一スレッドの時刻を戻さない。
    previous = std::max(previous, candidate);
    if (sample)
        *sample = {qpc, previous,
                   old.qpc && old.qpc != cached.qpc ? candidate - old.AtTicks(qpc) : 0,
                   previous - candidate, cached.ppm};
    return previous;
}

} // namespace timer
} // namespace core
} // namespace cccaster
