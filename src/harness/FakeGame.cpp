// ============================================================================
// FakeGame.cpp — スクリプトされた MBAA（実装）
// ============================================================================

#include "harness/FakeGame.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/sync/MatchInputBuffer.hpp"

#include <cstdio>

namespace cccaster::harness {

FakeGame::FakeGame(const Script &script) : _script(script) {
    _gameMode = CC_GAME_MODE_MAIN; // 25: メインメニュー
}

const char *FakeGame::StageName() const {
    switch (_stage) {
    case Stage::Boot:
        return "Boot";
    case Stage::CharaSelect:
        return "CharaSelect";
    case Stage::Loading:
        return "Loading";
    case Stage::InGame:
        return "InGame";
    case Stage::Rematch:
        return "Rematch";
    case Stage::Finished:
        return "Finished";
    }
    return "?";
}

void FakeGame::EnterStage(Stage next) {
    _stage = next;
    _framesInStage = 0;

    switch (next) {
    case Stage::Boot:
        _gameMode = CC_GAME_MODE_MAIN;
        break;
    case Stage::CharaSelect:
        _gameMode = CC_GAME_MODE_CHARA_SELECT;
        _introState = 0;
        break;
    case Stage::Loading:
        _gameMode = CC_GAME_MODE_LOADING;
        break;
    case Stage::InGame:
        _gameMode = CC_GAME_MODE_IN_GAME;
        _introState = 2; // イントロ演出中から始まる
        _realTimer = 0;
        break;
    case Stage::Rematch:
        _gameMode = CC_GAME_MODE_RETRY;
        _introState = 0;
        ++_menuStateCounter;
        break;
    case Stage::Finished:
        break;
    }
}

void FakeGame::Advance() {
    if (_stage == Stage::Finished)
        return;

    ++_frame;
    ++_worldTimer; // 実ゲーム同様、常時カウントアップ
    ++_framesInStage;

    switch (_stage) {
    case Stage::Boot:
        if (_framesInStage >= _script.mainMenuFrames)
            EnterStage(Stage::CharaSelect);
        break;

    case Stage::CharaSelect:
        if (_framesInStage >= _script.charaSelectFrames)
            EnterStage(Stage::Loading);
        break;

    case Stage::Loading:
        if (_framesInStage >= _script.loadingFrames)
            EnterStage(Stage::InGame);
        break;

    case Stage::InGame: {
        const uint32_t introEnd = _script.introPlayFrames;
        const uint32_t preEnd = introEnd + _script.introPreFrames;
        const uint32_t roundEnd = preEnd + _script.roundFrames;

        if (_framesInStage <= introEnd) {
            _introState = 2; // イントロ演出中
        } else if (_framesInStage <= preEnd) {
            _introState = 1; // pre-game
        } else if (_framesInStage <= roundEnd) {
            _introState = 0; // 対戦進行中
            ++_realTimer;
        } else {
            ++_roundsPlayed;
            if (_roundsPlayed >= _script.rounds) {
                EnterStage(Stage::Rematch);
            } else {
                // 次ラウンド: イントロからやり直し
                _framesInStage = 0;
                _introState = 2;
                _realTimer = 0;
            }
        }
        break;
    }

    case Stage::Rematch:
        if (_framesInStage >= _script.rematchFrames)
            EnterStage(Stage::Finished);
        break;

    case Stage::Finished:
        break;
    }
}

void FakeGame::WriteInput(GameInput p1, GameInput p2) {
    // 配信元のネットプレイフレームを一緒に記録する。プロセスごとに進行が
    // ずれても、このフレーム番号で突き合わせれば入力列を比較できる。
    const uint32_t netFrame = cccaster::core::sync::MatchInputBuffer::GetInstance().GetReadPos();
    _written.push_back(Record{_frame, netFrame, _gameMode, _introState, p1, p2});
}

void FakeGame::SampleState(uint32_t netFrame) {
    _states.push_back(StateSample{netFrame, _gameMode, _introState, _worldTimer, _realTimer});
}

bool FakeGame::DumpStatesTo(const std::string &path) const {
    FILE *fp = std::fopen(path.c_str(), "w");
    if (!fp)
        return false;
    std::fprintf(fp, "# netFrame mode intro WT RT\n");
    for (const StateSample &s : _states) {
        std::fprintf(fp, "%u %u %u %u %u\n", s.netFrame, s.gameMode, static_cast<unsigned>(s.introState),
                     s.worldTimer, s.realTimer);
    }
    std::fclose(fp);
    return true;
}

bool FakeGame::DumpTo(const std::string &path) const {
    FILE *fp = std::fopen(path.c_str(), "w");
    if (!fp)
        return false;

    // netFrame を先頭に置く。プロセスごとに進行がずれても、この番号で
    //突き合わせれば「同じネットプレイフレームに同じ入力が配られたか」を比較できる。
    std::fprintf(fp, "# netFrame p1dir p1btn p2dir p2btn | frame gameMode intro\n");
    for (const Record &r : _written) {
        std::fprintf(fp, "%u %u %u %u %u | %u %u %u\n", r.netFrame, static_cast<unsigned>(r.p1.direction),
                     static_cast<unsigned>(r.p1.buttons), static_cast<unsigned>(r.p2.direction),
                     static_cast<unsigned>(r.p2.buttons), r.frame, r.gameMode,
                     static_cast<unsigned>(r.introState));
    }
    std::fclose(fp);
    return true;
}

} // namespace cccaster::harness
