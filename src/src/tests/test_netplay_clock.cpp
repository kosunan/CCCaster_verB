// ============================================================================
// test_netplay_clock.cpp — NetplayClock（θ推定・α補正）の特性化テスト
//
// 【目的】
//   NTP T1-T4 の計算、最小RTTフィルタ、α補正カーブを固定する。
//   ここが狂うとメトロノームの歩調が狂い、症状は「ロードでずれる」
//   「対戦中に片側だけ加速する」として現れるため、数値で押さえておく。
//
// 【依存】
//   NetplayClock.cpp は WasapiClock::GetTimeUs() だけを外部に要求する。
//   stub_wasapi_clock.cpp で置換しており、実時刻には一切依存しない。
// ============================================================================

#include "test_support.hpp"
#include "core_dll/sync/NetplayClock.hpp"
#include "core_dll/timing/FrameCadence.hpp"

using cccaster::core::timer::NetplayClock;

// ============================================================================
// θ / RTT の計算
// ============================================================================

static void Ntp_ComputesRttAndThetaFromT1ToT4() {
    CC_CASE("NetplayClock: RTT=(T4-T1)-(T3-T2), θ=((T2-T1)+(T3-T4))/2");
    NetplayClock c;
    // 相手の時計が +1000us 進んでおり、片道 500us の理想ケース
    //   T1=0 送信 / T2=1500 相手受信 / T3=1600 相手送信 / T4=1100 自分受信
    //   RTT = (1100-0) - (1600-1500) = 1000
    //   θ   = ((1500-0) + (1600-1100)) / 2 = 1000
    c.AddNtpSample(0, 1500, 1600, 1100);

    CC_CHECK_EQ(c.GetRttUs(), 1000);
    CC_CHECK_EQ(c.GetThetaUs(), 1000);
}

static void Ntp_RejectsNegativeRttSamples() {
    CC_CASE("NetplayClock: RTT が負のサンプルは破棄する");
    NetplayClock c;
    c.AddNtpSample(0, 1500, 1600, 1100); // 有効: RTT=1000
    c.AddNtpSample(0, 5000, 9000, 1000); // RTT = 1000 - 4000 = -3000 → 破棄

    CC_CHECK_EQ(c.GetRttUs(), 1000);
    CC_CHECK_EQ(c.GetThetaUs(), 1000);
}

static void Ntp_AdoptsThetaOfMinimumRttSample() {
    CC_CASE("NetplayClock: 最小RTTサンプルのθを採用する");
    NetplayClock c;
    // 1本目: RTT=10000 の劣悪サンプル（θ=5000）
    c.AddNtpSample(0, 10000, 10000, 10000);
    CC_CHECK_EQ(c.GetRttUs(), 10000);
    CC_CHECK_EQ(c.GetThetaUs(), 5000);

    // 2本目: RTT=1000 の良サンプル（θ=1000）→ こちらを採用
    c.AddNtpSample(0, 1500, 1600, 1100);
    CC_CHECK_EQ(c.GetRttUs(), 1000);
    CC_CHECK_EQ(c.GetThetaUs(), 1000);

    // 3本目: 再び劣悪 → 最小RTTは維持される
    c.AddNtpSample(0, 20000, 20000, 20000);
    CC_CHECK_EQ(c.GetRttUs(), 1000);
    CC_CHECK_EQ(c.GetThetaUs(), 1000);
}

// ============================================================================
// θ安定判定（Counting へ移行する条件）
// ============================================================================

static void ThetaStable_RequiresMinimumSampleCount() {
    CC_CASE("NetplayClock: サンプル数が STABLE_MIN 未満なら不安定扱い");
    NetplayClock c;
    for (int i = 0; i < NetplayClock::STABLE_MIN - 1; i++) {
        c.AddNtpSample(0, 1500, 1600, 1100);
    }
    CC_CHECK(!c.IsThetaStable());

    c.AddNtpSample(0, 1500, 1600, 1100);
    CC_CHECK(c.IsThetaStable());
}

static void ThetaStable_FalseWhenSamplesScatter() {
    CC_CASE("NetplayClock: θのばらつきが大きいと不安定と判定する");
    NetplayClock c;
    // θ を 0 と 20000us で交互に振らせる（σ >> STABLE_SIGMA=1000）
    for (int i = 0; i < NetplayClock::STABLE_MIN + 2; i++) {
        if (i % 2 == 0)
            c.AddNtpSample(0, 500, 600, 1000); // θ=50
        else
            c.AddNtpSample(0, 20500, 20600, 1000); // θ=20050
    }
    CC_CHECK(!c.IsThetaStable());
}

// ============================================================================
// α補正カーブ（3段階: デッドバンド → 二乗 → 飽和）
// ============================================================================

/// θ を狙った値にした上で baseline=0 の時計を作る。
/// T1=0, T4=2*owd とし、T2=T3=owd+theta にすると RTT=2*owd, θ=theta になる。
static void FeedTheta(NetplayClock &c, int64_t theta, int64_t owd = 500) {
    c.AddNtpSample(1000000,1000000+owd+theta,1000000+owd+theta,1000000+2*owd);
}

static void Alpha_NoCorrectionInsideDeadBand() {
    CC_CASE("NetplayClock: |Δθ| がデッドバンド内なら補正なし");
    NetplayClock c;
    for (int i = 0; i < 3; i++)
        FeedTheta(c, NetplayClock::DEAD_BAND_US - 1);

    CC_CHECK_EQ(c.GetTickUs(), NetplayClock::BASE_TICK_US);
}

static void Alpha_SaturatesBeyondStrongThreshold() {
    CC_CASE("NetplayClock: |Δθ| が飽和閾値を超えたら最大補正");
    NetplayClock c;
    // 相手が先行（Δθ>0）→ 自分を速く = ティックを短く
    for (int i = 0; i < 3; i++)
        FeedTheta(c, NetplayClock::STRONG_TH_US + 10000);

    CC_CHECK_EQ(c.GetTickUs(), NetplayClock::BASE_TICK_US - NetplayClock::MAX_ALPHA_US);
}

static void Alpha_SignFollowsDriftDirection() {
    CC_CASE("NetplayClock: Δθ の符号でティックの伸縮方向が決まる");
    NetplayClock c;
    // 相手が遅延（Δθ<0）→ 自分を遅く = ティックを長く
    for (int i = 0; i < 3; i++)
        FeedTheta(c, -(NetplayClock::STRONG_TH_US + 10000));

    CC_CHECK_EQ(c.GetTickUs(), NetplayClock::BASE_TICK_US + NetplayClock::MAX_ALPHA_US);
}

static void Alpha_UsesDeltaFromBaselineNotAbsoluteTheta() {
    CC_CASE("NetplayClock: α補正はベースラインθからの差分で決まる");
    NetplayClock c;
    // 大きな絶対θ（時計そのものがずれている）を積んでからベースライン確定
    for (int i = 0; i < 3; i++)
        FeedTheta(c, 100000);
    c.SetBaselineTheta();

    // 絶対θは 100000 のままだが Δθ=0 なので補正は入らない
    CC_CHECK_EQ(c.GetBaselineTheta(), 100000);
    CC_CHECK_EQ(c.GetTickUs(), NetplayClock::BASE_TICK_US);
}

static void Alpha_NoCorrectionWithTooFewSamples() {
    CC_CASE("NetplayClock: サンプル3本未満では補正しない");
    NetplayClock c;
    FeedTheta(c, NetplayClock::STRONG_TH_US * 2);
    FeedTheta(c, NetplayClock::STRONG_TH_US * 2);

    CC_CHECK_EQ(c.GetTickUs(), NetplayClock::BASE_TICK_US);
}

// ============================================================================
// スタート時刻の合意
// ============================================================================

static void AgreedStartTime_TakesLaterOfBothSides() {
    CC_CASE("NetplayClock: 合意開始時刻は両者の遅い方");
    NetplayClock c; // θ=0（サンプルなし）
    c.SetLocalStartTime(5000);
    c.SetPeerStartTime(8000);

    CC_CHECK_EQ(c.GetAgreedStartTime(), 8000);
}

static void AgreedStartTime_ZeroUntilBothSidesKnown() {
    CC_CASE("NetplayClock: 片側だけでは合意時刻を返さない");
    NetplayClock c;
    CC_CHECK_EQ(c.GetAgreedStartTime(), 0);

    c.SetLocalStartTime(5000);
    CC_CHECK_EQ(c.GetAgreedStartTime(), 0);
}

static void PeerStartTime_IsConvertedToLocalClockByTheta() {
    CC_CASE("NetplayClock: 相手の開始時刻はθでローカル基準に変換される");
    NetplayClock c;
    c.AddNtpSample(0, 1500, 1600, 1100); // θ=1000
    c.SetLocalStartTime(5000);
    c.SetPeerStartTime(8000); // → 8000 - 1000 = 7000

    CC_CHECK_EQ(c.GetAgreedStartTime(), 7000);
}

static void PeerStartTime_ConversionUsesThetaAtCallTime() {
    CC_CASE("[HAZARD] NetplayClock: θ変換は SetPeerStartTime 実行時の値で固定される");
    // 後からθが更新されても、変換済みの _peerStartTimeUs は再計算されない。
    // START 合意の前後でθが動くと、開始時刻が片側だけずれる。
    NetplayClock c;
    c.AddNtpSample(0, 1500, 1600, 1100); // RTT=1000, θ=1000
    c.SetLocalStartTime(1000);
    c.SetPeerStartTime(8000); // → 8000 - 1000 = 7000 で確定

    c.AddNtpSample(0, 300, 300, 400); // RTT=400 (より良い), θ=100 に更新
    CC_CHECK_EQ(c.GetThetaUs(), 100);
    CC_CHECK_EQ(c.GetAgreedStartTime(), 7000); // 7900 に再計算はされない
}

static void MinRtt_TieKeepsEarlierSample() {
    CC_CASE("[HAZARD] NetplayClock: RTT 同値ではθを新しいサンプルに乗り換えない");
    // 最小RTT探索が strict less-than のため、RTT が安定した回線では
    // 最初に記録されたθを掴んだまま更新されなくなる。
    NetplayClock c;
    c.AddNtpSample(0, 1500, 1600, 1100); // RTT=1000, θ=1000
    c.AddNtpSample(0, 600, 600, 1000);   // RTT=1000（同値）, θ=100

    CC_CHECK_EQ(c.GetRttUs(), 1000);
    CC_CHECK_EQ(c.GetThetaUs(), 1000); // 100 にはならない
}

static void StartTime_ZeroIsTreatedAsUnset() {
    CC_CASE("[HAZARD] NetplayClock: 開始時刻 0 は「未設定」と区別できない");
    // ミスマッチフレームと同じセンチネル 0 の問題。
    // 実運用では WASAPI 時刻が 0 にならないため顕在化しないが、
    // テストやリプレイで時刻を 0 起点にすると合意が成立しない。
    NetplayClock c;
    c.SetLocalStartTime(0);
    c.SetPeerStartTime(8000);

    CC_CHECK_EQ(c.GetAgreedStartTime(), 0);
}

// ============================================================================
// リセット
// ============================================================================

static void Reset_ClearsSamplesAndBaselineTheta() {
    CC_CASE("NetplayClock: Reset は baselineTheta と時計モデルも消す");
    // セッションを再開しても古い基準差とドリフトを持ち越さない。
    NetplayClock c;
    for (int i = 0; i < 3; i++)
        FeedTheta(c, 5000);
    c.SetBaselineTheta();
    CC_CHECK_EQ(c.GetBaselineTheta(), 5000);

    c.Reset();

    CC_CHECK_EQ(c.GetThetaUs(), 0);
    CC_CHECK_EQ(c.GetBaselineTheta(), 0);
    CC_CHECK_EQ(c.GetAgreedStartTime(), 0);
}

// ============================================================================

int main() {
    CC_CASE("ジッターが大きくても入力バリア用のペース推定は開始できる");
    cccaster::core::timer::NetplayClock estimate;
    estimate.Reset();
    CC_CHECK(!estimate.HasTimingEstimate());
    for (int i = 0; i < 10; ++i) {
        const int64_t offset = (i % 2) ? 20000 : -20000;
        estimate.AddNtpSample(100000, 160000 + offset, 170000 + offset, 230000);
    }
    CC_CHECK(estimate.HasTimingEstimate());
    CC_CHECK(!estimate.IsThetaStable());
    Ntp_ComputesRttAndThetaFromT1ToT4();
    Ntp_RejectsNegativeRttSamples();
    Ntp_AdoptsThetaOfMinimumRttSample();

    ThetaStable_RequiresMinimumSampleCount();
    ThetaStable_FalseWhenSamplesScatter();

    Alpha_NoCorrectionInsideDeadBand();
    Alpha_SaturatesBeyondStrongThreshold();
    Alpha_SignFollowsDriftDirection();
    Alpha_UsesDeltaFromBaselineNotAbsoluteTheta();
    Alpha_NoCorrectionWithTooFewSamples();

    AgreedStartTime_TakesLaterOfBothSides();
    AgreedStartTime_ZeroUntilBothSidesKnown();
    PeerStartTime_IsConvertedToLocalClockByTheta();
    PeerStartTime_ConversionUsesThetaAtCallTime();
    MinRtt_TieKeepsEarlierSample();
    StartTime_ZeroIsTreatedAsUnset();

    Reset_ClearsSamplesAndBaselineTheta();

    using namespace cccaster::core::timer;
    CC_CASE("1/60µsのNTP時刻を整数µsへ丸めず保持する");
    NetplayClock fractional;
    fractional.AddNtpSampleTicks(60000001,60030008,60030008,60060001);
    CC_CHECK_EQ(fractional.GetThetaTicks(),7);
    CC_CHECK_EQ(fractional.PeerToLocalTicks(90000008,90000001),90000001);
    constexpr int W=PeerClockModel::WindowSeconds;
    CC_CASE("独立窓の学習後は正負の小数速度差と原点を保持する");
    for (const double ppm : {-250.0,-20.0,-0.25,0.0,0.25,20.0,250.0}) {
        PeerClockModel model;
        const auto offset=[&](int sec) {return 600007+int64_t(std::llround(sec*ppm*60));};
        for(int sec=0;sec<=2*W;++sec) model.AddTicks(int64_t(sec)*ClockSecond,offset(sec),60000);
        CC_CHECK(model.Ready(2*W*ClockSecond));
        CC_CHECK(std::abs(model.DriftRate(2*W*ClockSecond)*1e6-ppm)<0.002);
        CC_CHECK(std::abs(model.OffsetTicks(3*W*ClockSecond)-offset(3*W))<=2);
        const auto rev=model.Revision();
        const auto predicted=model.OffsetTicks(4*W*ClockSecond);
        for(int sec=2*W+1;sec<=4*W;++sec)
            model.AddTicks(int64_t(sec)*ClockSecond,offset(sec)+(sec%2?900:-900),60000);
        CC_CHECK_EQ(model.Revision(),rev);
        CC_CHECK_EQ(model.OffsetTicks(4*W*ClockSecond),predicted);
        CC_CHECK(model.Ready(4*W*ClockSecond));
        model.Reset(); CC_CHECK(!model.Ready(4*W*ClockSecond));
    }
    CC_CASE("パケット集中・雑音だけではモデルをロックしない");
    PeerClockModel burst;
    for(int i=0;i<10000;++i) burst.AddTicks(ClockSecond+i,600007,60000);
    CC_CHECK(!burst.Ready(2*ClockSecond));
    PeerClockModel noisy;
    const int noise[]={1800,-1200,900,-1600,1400,-800,700,-1000};
    for(int sec=0;sec<=5*W;++sec) noisy.AddTicks(int64_t(sec)*ClockSecond,600000+noise[sec%8]*60,2400000);
    CC_CHECK(!noisy.Ready(5*W*ClockSecond));
    CC_CASE("保持中の通信断でモデルを即変更せず、検証期限後は失効する");
    PeerClockModel held;
    for(int sec=0;sec<=2*W;++sec) held.AddTicks(int64_t(sec)*ClockSecond,600007,60000);
    CC_CHECK(held.Ready(4*W*ClockSecond));
    CC_CHECK(held.Status(4*W*ClockSecond)==PeerClockModel::State::Holdover);
    CC_CHECK(!held.Ready((2*W+PeerClockModel::HoldoverSeconds+1)*ClockSecond));
    held.AddTicks(8*W*ClockSecond,600007,60000);
    CC_CHECK(!held.Ready(8*W*ClockSecond));
    CC_CASE("持続した推定変化は1窓では採用せず2つの独立窓で再評価する");
    PeerClockModel changed;
    for(int sec=0;sec<=2*W;++sec) changed.AddTicks(int64_t(sec)*ClockSecond,600007,60000);
    const auto old=changed.Revision();
    for(int sec=2*W+1;sec<=4*W;++sec) changed.AddTicks(int64_t(sec)*ClockSecond,780007,60000);
    CC_CHECK_EQ(changed.Revision(),old);
    for(int sec=4*W+1;sec<=5*W;++sec) changed.AddTicks(int64_t(sec)*ClockSecond,780007,60000);
    CC_CHECK(changed.Revision()>old);
    CC_CHECK(std::abs(changed.OffsetTicks(5*W*ClockSecond)-780007)<=1);
    CC_CASE("小数速度補正を1時間加算しても整数µsの丸め誤差を蓄積しない");
    for (const int scale : {1,4}) for(const double ppm : {-250.0,-20.0,-0.25,0.25,20.0,250.0}) {
        PeerClockModel model;
        for(int sec=0;sec<=2*W;++sec)
            model.AddTicks(int64_t(sec)*ClockSecond,600007+int64_t(std::llround(sec*ppm*60)),60000);
        FrameCadence cadence; cadence.ResetTicks(17);
        const auto correction=model.PeriodCorrectionParts(2*W*ClockSecond);
        for(int frame=0;frame<216000;++frame) cadence.AdvanceCorrected(correction,scale);
        const auto ideal=17+int64_t(std::floor(216000.0*ClockFrame/(1+ppm/1e6)/scale));
        CC_CHECK(std::abs(cadence.NextTicks()-ideal)<=1);
    }
    return cccaster::test::Summarize("netplay_clock");
}
