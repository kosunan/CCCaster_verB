#pragma once
#include "core_dll/timing/ClockUnits.hpp"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cccaster::core::timer {
// 重ならない16秒窓を比較して時計を採用する。採用後は標本の入替で動かさない。
// uncertaintyは観測ノイズからの判定幅であり、固定片道差の上限保証ではない。
class PeerClockModel {
  public:
    static constexpr int WindowSeconds = 16;
    static constexpr int HoldoverSeconds = 48;
    enum class State { Learning, Locked, Rechecking, Holdover };
    void Reset() { *this = PeerClockModel{}; }
    void AddTicks(int64_t local, int64_t theta, int64_t rtt) {
        if (local < 0 || rtt < 0 || rtt > ClockSecond || local < lastSample_) return;
        const auto second = local / ClockSecond;
        const auto window = second / WindowSeconds;
        if (window_ >= 0 && window != window_) {
            if (window == window_ + 1) Evaluate(local);
            else candidate_ = {};
            bins_ = {};
        }
        window_ = window;
        lastSample_ = local;
        auto &bin = bins_[second % bins_.size()];
        if (!bin.valid || rtt < bin.rtt) bin = {local, theta, rtt, true};
        if (held_.valid && local - validatedAt_ > HoldoverSeconds * ClockSecond) {
            held_ = candidate_ = {};
            ++revision_;
        }
    }
    bool Ready(int64_t now) const {
        return held_.valid && now >= held_.anchor && now - validatedAt_ <= HoldoverSeconds * ClockSecond;
    }
    State Status(int64_t now) const {
        if (!Ready(now)) return State::Learning;
        if (now - validatedAt_ > (WindowSeconds+2) * ClockSecond) return State::Holdover;
        return candidate_.valid ? State::Rechecking : State::Locked;
    }
    static const char *Name(State state) {
        switch (state) {
        case State::Locked: return "locked";
        case State::Rechecking: return "rechecking";
        case State::Holdover: return "holdover";
        default: return "learning";
        }
    }
    int64_t OffsetTicks(int64_t now) const { return Offset(held_, now); }
    double DriftRate(int64_t now) const { return Ready(now) ? held_.rate : 0; }
    int64_t PeriodCorrectionParts(int64_t now) const {
        const auto rate = DriftRate(now);
        return int64_t(std::llround(-rate / (1 + rate) * ClockFrame * ClockParts));
    }
    uint32_t Revision() const { return revision_; }
    uint32_t Evaluations() const { return evaluations_; }
    int WindowCount() const { return lastFit_.count; }
    int64_t WindowUncertaintyTicks() const { return int64_t(std::ceil(lastFit_.uncertainty)); }
    int64_t WindowRateErrorPpb() const { return int64_t(std::ceil(lastFit_.rateError*1e9)); }
    int64_t ResidualTicks() const { return residual_; }
    int64_t UncertaintyTicks() const { return int64_t(std::ceil(held_.uncertainty)); }
    int64_t RateUncertaintyPpb() const { return int64_t(std::ceil(held_.rateError * 1e9)); }

  private:
    struct Bin { int64_t local = 0, theta = 0, rtt = 0; bool valid = false; };
    struct Fit {
        int64_t anchor = 0, offset = 0;
        double rate = 0, uncertainty = 0, rateError = 0;
        bool valid = false;
        int count = 0;
    };
    static int64_t Offset(const Fit &fit, int64_t now) {
        return fit.offset + int64_t(std::llround(double(now - fit.anchor) * fit.rate));
    }
    Fit Regression() const {
        int64_t floor = INT64_MAX, anchor = 0, first = INT64_MAX;
        for (const auto &b : bins_) if (b.valid) {
            floor = std::min(floor, b.rtt);
        }
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int count = 0;
        const auto usable = [&](const Bin &b) { return b.valid && b.rtt <= floor + 500 * 60; };
        for (const auto &b : bins_) if (usable(b)) anchor = std::max(anchor, b.local);
        for (const auto &b : bins_) if (usable(b)) {
            const auto x = double(b.local - anchor) / ClockSecond;
            sx += x; sy += double(b.theta); sxx += x*x; sxy += x*double(b.theta);
            first = std::min(first, b.local);
            ++count;
        }
        if (count < 4 || anchor - first < 3 * ClockSecond) { Fit sparse; sparse.count=count; return sparse; }
        const auto den = sxx - sx*sx/count;
        if (den <= 0) return {};
        const auto slope = (sxy - sx*sy/count) / den;
        const auto intercept = (sy - slope*sx) / count;
        double squares = 0;
        for (const auto &b : bins_) if (usable(b)) {
            const auto error = double(b.theta) - intercept - slope*double(b.local-anchor)/ClockSecond;
            squares += error*error;
        }
        const auto variance = std::max(1.0, squares / (count-2));
        const auto rateError = std::sqrt(variance/den) / ClockSecond;
        const auto uncertainty = std::sqrt(variance * (1.0/count + sx*sx/(count*count*den)));
        // 雑音下の傾きを速度差へ即採用しない。真の小数ppmも有意なら残す。
        const auto rate = std::abs(slope) > 3*std::sqrt(variance/den) ? slope/ClockSecond : 0.0;
        const bool valid = std::abs(rate)<=0.0002501 && uncertainty+rateError*8*ClockSecond<=250*60;
        return {anchor,int64_t(std::llround(intercept)),std::clamp(rate,-0.00025,0.00025),
                uncertainty,rateError,valid,count};
    }
    static bool Consistent(const Fit &a, const Fit &b) {
        const auto seconds = double(b.anchor-a.anchor)/ClockSecond;
        const auto allowance = std::max(100.0*60, 3*(a.uncertainty+b.uncertainty+
                                                    std::abs(seconds)*ClockSecond*a.rateError));
        const auto rateAllowance = std::max(0.000005, 3*(a.rateError+b.rateError));
        return std::abs(double(Offset(a,b.anchor)-b.offset)) <= allowance &&
               std::abs(a.rate-b.rate) <= rateAllowance;
    }
    void Evaluate(int64_t now) {
        ++evaluations_;
        const auto fit = lastFit_ = Regression();
        if (!fit.valid) { candidate_ = {}; return; }
        residual_ = held_.valid ? fit.offset-Offset(held_,fit.anchor) : 0;
        if (held_.valid && Ready(now)) {
            const auto allowance = std::max(250.0*60, 4*fit.uncertainty);
            // 検証だけ。問題のない新標本へモデルを再フィットしない。
            if (std::abs(double(residual_)) <= allowance &&
                std::abs(fit.rate-held_.rate) <= std::max(0.000005,4*fit.rateError)) {
                validatedAt_ = now;
                candidate_ = {};
                return;
            }
        }
        if (candidate_.valid && Consistent(candidate_,fit)) {
            // 2つの独立窓が一致したときだけ採用。適用側で位相/速度を緩やかに移す。
            const auto rate = (candidate_.rate+fit.rate)/2;
            const auto offset = (Offset(candidate_,fit.anchor)+fit.offset)/2;
            held_ = {fit.anchor,offset,rate,std::max(candidate_.uncertainty,fit.uncertainty),
                     std::max(candidate_.rateError,fit.rateError),true};
            validatedAt_ = now;
            candidate_ = {};
            ++revision_;
        } else candidate_ = fit;
    }
    std::array<Bin,WindowSeconds> bins_{};
    Fit held_{}, candidate_{}, lastFit_{};
    int64_t window_ = -1, lastSample_ = 0, validatedAt_ = 0, residual_ = 0;
    uint32_t revision_ = 0, evaluations_ = 0;
};
}

