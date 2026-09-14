#include "test_support.hpp"
#include "core_dll/ui/UtilityMetrics.hpp"
#include <limits>

using namespace cccaster::domain::ui;

int main() {
    CC_CASE("実RBは設定上限でなく開始イベントを実時間1秒で集計");
    RollbackMetricsWindow rb;
    CC_CHECK(!rb.Read(0).available);
    rb.Reset(true);
    CC_CHECK(rb.Read(0).available);
    CC_CHECK_EQ(rb.Read(0).count, 0);
    rb.Record(100, 2);
    rb.Record(100, 4);
    rb.Record(200, 1);
    CC_CHECK_EQ(rb.Read(1000).count, 3);
    CC_CHECK_EQ(rb.Read(1000).maxDepth, 4);
    CC_CHECK_EQ(rb.Read(1100).count, 1);
    CC_CHECK_EQ(rb.Read(1100).maxDepth, 1);
    CC_CHECK_EQ(rb.Read(1200).count, 0);
    rb.Record(1100, 3); // 同じバケットの前の1秒を上書き
    CC_CHECK_EQ(rb.Read(1200).count, 1);
    CC_CHECK_EQ(rb.Read(1200).maxDepth, 3);
    rb.Record(1200, 0);
    CC_CHECK_EQ(rb.Read(1200).count, 1);
    rb.Reset(false);
    rb.Record(1200, 4);
    CC_CHECK(!rb.Read(1200).available);
    CC_CHECK_EQ(rb.Read(1200).count, 0);

    CC_CASE("RTTは受理サンプルを1回、未計測とジッタ0と失効を区別");
    NetworkMetricsWindow net;
    CC_CHECK(!net.Read(0).available);
    net.Record(0, 0);
    CC_CHECK(net.Read(0).available);
    CC_CHECK(!net.Read(0).jitterAvailable);
    net.Record(100, 20);
    net.Record(200, 10);
    auto value = net.Read(200);
    CC_CHECK_EQ(value.latestRttMs, 10);
    CC_CHECK_EQ(value.maxRttMs, 20);
    CC_CHECK_EQ(value.jitterMs, 15);
    CC_CHECK_EQ(value.sampleCount, 3);
    CC_CHECK(value.jitterAvailable);
    for (int i = 0; i < 60; ++i) net.Read(200); // HUD再描画はサンプルを増やさない
    CC_CHECK_EQ(net.Read(200).sampleCount, 3);
    value = net.Read(1000);
    CC_CHECK_EQ(value.sampleCount, 2);
    CC_CHECK_EQ(value.jitterMs, 10);
    value = net.Read(1200);
    CC_CHECK(value.available && value.stale);
    CC_CHECK(!value.jitterAvailable);
    CC_CHECK_EQ(value.sampleCount, 0);
    CC_CHECK_EQ(value.latestRttMs, 10);
    net.Record(1250, 30);
    CC_CHECK(!net.Read(1250).stale);
    CC_CHECK(!net.Read(1250).jitterAvailable); // 失効した古いRTTとの差は使わない
    net.Record(1300, 30);
    CC_CHECK(net.Read(1300).jitterAvailable);
    CC_CHECK_EQ(net.Read(1300).jitterMs, 0);
    net.Record(1400, std::numeric_limits<float>::infinity());
    net.Record(1400, -1);
    CC_CHECK_EQ(net.Read(1400).sampleCount, 2);
    net.Reset();
    CC_CHECK(!net.Read(1400).available);

    CC_CASE("D+Rで覆えないRTTスパイクを8秒保持し超過量で色分けする");
    LatencyWarningTracker latency;
    latency.Record(100, 110.0f);
    auto alert = latency.Read(100, 2, 4); // 100msを約1F超過
    CC_CHECK(alert.evaluated);
    CC_CHECK(alert.severity == LatencyWarningSeverity::Light);
    CC_CHECK_EQ(alert.extraFrames, 1u);
    latency.Record(200, 140.0f); // 約2.4F超過
    alert = latency.Read(200, 2, 4);
    CC_CHECK(alert.severity == LatencyWarningSeverity::Heavy);
    CC_CHECK_EQ(alert.extraFrames, 3u);
    latency.Record(300, 20.0f); // 通常値でスパイク保持時間を延長しない
    CC_CHECK(latency.Read(8199, 2, 4).severity == LatencyWarningSeverity::Heavy);
    CC_CHECK(latency.Read(8200, 2, 4).severity == LatencyWarningSeverity::None);
    LatencyWarningTracker covered;
    covered.Record(8300, 120.0f);
    CC_CHECK(covered.Read(8300, 3, 4).severity == LatencyWarningSeverity::Light);
    CC_CHECK(covered.Read(8300, 4, 4).severity == LatencyWarningSeverity::None);
    return cccaster::test::Summarize("utility_metrics");
}
