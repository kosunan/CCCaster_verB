#pragma once
// ============================================================================
// Platform — OS 依存処理の唯一の窓口
//
// 【なぜ存在するか】
//   実機用 DLL は Windows 専用だが、harness（ゲーム無しで同期ロジックを通しで
//   走らせる実行ファイル）は Linux でも動かしたい。両方でビルドしたいコードが
//   `windows.h` を直接叩いていると、その1行のためにファイル全体が Windows 専用に
//   なる。時刻取得・待機・CPU 緩和のような「OS ごとに書き方が違うだけで意味は同じ」
//   処理をここに集約し、呼び出し側から `#ifdef _WIN32` を消す。
//
// 【設計上の制約】
//   このヘッダは `windows.h` を include しない。Win32 の型が漏れると、
//   include した側が結局 Windows 専用になるため。実装は Platform.cpp に隠す。
//
// SleepMs/RealSleepMsはともに実時間でCPUを休止する。
// ゲームEXEのフレーム待機は命令分岐でバイパスし、OSの時計・Sleepを加工しない。
// 周期の精密待機にはPreciseWaitUsと上位の絶対締切を使う。
// ============================================================================

#include <cstdint>

namespace cccaster::platform {
// MMCSSへの参加は呼出しスレッドだけ。プロセス全体やOS設定は変更しない。
class TimingThread {
  public:
    explicit TimingThread(const char *role);
    ~TimingThread();
    void MaintainAffinity(); // フレーム準備時だけ。CPU0除外・診断用固定の範囲へ補正。
    TimingThread(const TimingThread &) = delete;
    TimingThread &operator=(const TimingThread &) = delete;

  private:
    void *library_ = nullptr, *handle_ = nullptr;
    uintptr_t previousAffinity_ = 0;
    uintptr_t guardedAffinity_ = 0;
    void *gameCoreLease_ = nullptr;
    unsigned affinityChanges_ = 0;
};

// ── 時刻 ───────────────────────────────────────────────────
/// 実時間の単調時刻 [μs]。
/// Windowsでは加工していないQPCを使う。
int64_t RealMonotonicUs();
// 1/60µs単位。QPCの分解能を整数µsへ落とさない。
int64_t RealMonotonicTicks();
// 診断用識別子。時計スピンの外側で取得する。
uint32_t ProcessId();
uint32_t ThreadId();

// ── 待機 ───────────────────────────────────────────────────
/// OSのSleep。
void SleepMs(uint32_t ms);

/// 実時間のSleep。CPUを明示的に手放したい箇所で使う。
void RealSleepMs(uint32_t ms);

// 高分解能待機タイマーでCPUを返し、最後の短区間だけ実QPCで確認する。
void PreciseWaitUs(int64_t durationUs);

/// スピンウェイト中に CPU に譲る（x86 では PAUSE 命令）。
void CpuRelax();

// ── タイマー分解能 ─────────────────────────────────────────
/// Windows のタイマー分解能を 1ms に上げる。Linux では何もしない。
/// 取得したら必ず EndHighResolutionTimers() で戻すこと。
void BeginHighResolutionTimers();
void EndHighResolutionTimers();

// ── 中断要求 ───────────────────────────────────────────────
/// ユーザーによる中断が要求されているか。
/// Windows: F12 の押下。Linux: SIGINT / SIGTERM を受けたか。
bool IsAbortRequested();

/// Linux でシグナルハンドラを登録する。Windows では何もしない。
/// harness の main 冒頭で呼ぶ。
void InstallAbortHandler();

// ── プロセス終了 ───────────────────────────────────────────
/// 自プロセスを即座に終了する（デストラクタも atexit も走らない）。
/// 実機では注入先の MBAA.exe ごと落ちる。
[[noreturn]] void TerminateSelf();

} // namespace cccaster::platform
