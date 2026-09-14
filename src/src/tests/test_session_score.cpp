#include "core_dll/engine/SessionScore.hpp"
#include "core_dll/engine/ReplayFileName.hpp"
#include "core_dll/engine/ReplayFileFormat.hpp"
#include <vector>
#include <cstdio>
#include <cstdlib>
using namespace cccaster::domain::session;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "失敗: %s:%d %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (false)
int main() {
    {
        using namespace cccaster::domain::session;
        auto names = ReplayFilePlayerNames("ReplayVS/20260915_010101_000_P1-ALICE[WIN]_P2-BOB.rep");
        CHECK(names[0] == "ALICE" && names[1] == "BOB");
        names = ReplayFilePlayerNames("20260915_010101_000_P1-ALICE_P2-BOB_[WIN]_2.rep");
        CHECK(names[0] == "ALICE" && names[1] == "BOB_");
        names = ReplayFilePlayerNames("20260915_010101_000_P1-ALICE_P2-BOB_[UNDECIDED].rep");
        CHECK(names[1] == "BOB");
        names = ReplayFilePlayerNames("legacy.rep");
        CHECK(names[0].empty() && names[1].empty());
    }

    CHECK(ReplayFileStem("20260915_010203", "Alice", "Bob", 1) == "20260915_010203_P1-Alice[WIN]_P2-Bob");
    CHECK(ReplayFileStem("20260915_010203", "Alice", "Bob", 2) == "20260915_010203_P1-Alice_P2-Bob[WIN]");
    CHECK(ReplayFileStem("t", "", "", 0) == "t_P1-PLAYER1_P2-PLAYER2_[UNDECIDED]");
    CHECK(ReplayPlayerName("../a:b*?\\c[WIN] .", "P") == ".._a_b___c_WIN_");
    CHECK(ReplayPlayerName(std::string(40, 'a'), "P").size() == 31);
    SessionScore score;
    std::vector<unsigned char> rep(0x60 + 0x8c + 20 + 0x90);
    std::memcpy(rep.data(), "MBAAReplayFile", 14); rep[0x5c] = 1;
    CHECK(ValidReplayFile(rep, 1));
    CHECK(!ValidReplayFile(rep, 2));
    rep.pop_back(); CHECK(!ValidReplayFile(rep, 1)); rep.push_back(0);
    rep[0x60 + 0x8c + 3] = 0xff; CHECK(!ValidReplayFile(rep, 1));
    score.Reset(true);
    const MatchResultFacts p1{2, 1, 2, true}, p2{0, 2, 2, true};
    CHECK(!score.Observe(ScoreScene::Result, 1, p1, true)); // 起動時の残留画面
    score.Observe(ScoreScene::Battle, 10, {}, true);
    CHECK(!score.Observe(ScoreScene::Battle, 20, p1, true)); // ラウンド世代変更
    CHECK(!score.Observe(ScoreScene::Result, 20, p2, false)); // 誤予測
    CHECK(score.Observe(ScoreScene::Result, 20, p1, true));
    CHECK(score.Snapshot().p1Wins == 1 && score.Snapshot().p2Wins == 0);
    CHECK(!score.Observe(ScoreScene::Result, 20, p1, true)); // 再送・同じ画面
    score.Observe(ScoreScene::Battle, 10, {}, true); // 旧世代再計算
    CHECK(!score.Observe(ScoreScene::Result, 20, p1, true));
    score.Observe(ScoreScene::Battle, 30, {}, true);
    CHECK(score.Observe(ScoreScene::Result, 40, p2, true)); // ONCEで次戦
    CHECK(score.Snapshot().p1Wins == 1 && score.Snapshot().p2Wins == 1);
    score.Observe(ScoreScene::Battle, 50, {}, true);
    score.Observe(ScoreScene::CharacterSelect, 60, {}, true);
    CHECK(!score.Observe(ScoreScene::Result, 70, p1, true)); // 中断
    score.Observe(ScoreScene::Battle, 80, {}, true);
    CHECK(score.Observe(ScoreScene::Result, 90, {2, 2, 2, true}, true)); // 同時到達は未裁定
    score.Observe(ScoreScene::Battle, 100, {}, true);
    CHECK(score.Observe(ScoreScene::Result, 110, {1, 0, 2, true}, true)); // 未到達を勝利としない
    score.Observe(ScoreScene::Battle, 120, {}, true);
    CHECK(score.Observe(ScoreScene::Result, 130, {}, true)); // 読取り不可
    CHECK(score.Snapshot().unresolved == 3 && score.Snapshot().revision == 5);
    score.Reset(false);
    score.Observe(ScoreScene::Battle, 200, {}, true);
    CHECK(!score.Observe(ScoreScene::Result, 210, p1, true)); // Training対象外
    CHECK(score.Snapshot().p1Wins == 0);
    score.Reset(true);
    score.Observe(ScoreScene::Battle, 1, {}, true);
    CHECK(score.Observe(ScoreScene::Result, 2, {1, 0, 1, true}, true));
    CHECK(score.Snapshot().p1Wins == 1); // 新セッションは初期値
    std::puts("確定試合スコア: 全条件合格");
}
