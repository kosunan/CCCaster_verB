// ============================================================================
// test_input_buffers.cpp — MatchInputBuffer のテスト
//
// 【MatchInputBuffer】
//   入力パイプライン再構築(2a)で安全化した後の「あるべき挙動」を定める。
//   以前 [HAZARD] として記録していた危険な挙動は、ここで正しい挙動の検証に
//   置き換わっている。対応は以下のとおり。
//
//     旧 [HAZARD]                          → 現在の要求
//     確定済みスロットの再確定を見逃す     → ConfirmConflicts() で観測できる
//     リング周回で別フレームを黙って返す   → FindSlot/TryRead が拒否する
//     ConfirmRemote が slot.frame を残す   → 常に frame と valid を立てる
//     ミスマッチ「なし」をフレーム0で表す  → HasMismatch/ConsumeMismatch(bool)
//     フレーム0が永久に読めない            → valid フラグで区別するので読める
//
// MenuInputBuffer はフレーム空間の一本化(2b)で撤去済み。入力は
// セッション通しの単一フレーム空間で MatchInputBuffer だけが扱う。
//
// 【依存】
//   ヘッダオンリーで依存ゼロ。ゲーム・DLL・通信を一切必要としない。
// ============================================================================

#include "test_support.hpp"
#include "core_dll/sync/MatchInputBuffer.hpp"

using cccaster::core::sync::MatchInputBuffer;

// ============================================================================
// MatchInputBuffer — 読取位置の算出
// ============================================================================

static void ReadPos_SubtractsDelayPlusRollback() {
    CC_CASE("MatchInputBuffer: readPos = writeHead - (delay + maxRollback)");
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(/*startFrame*/ 100, /*delay*/ 2, /*maxRollback*/ 4);

    CC_CHECK_EQ(b.GetWriteHead(), 100u);
    CC_CHECK_EQ(b.GetReadPos(), 94u);
}

static void ReadPos_ClampsOffsetToAtLeastOne() {
    CC_CASE("MatchInputBuffer: delay=0 rollback=0 でも最低1F遅れる");
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(100, 0, 0);

    CC_CHECK_EQ(b.GetReadPos(), 99u);
}

static void ReadPos_ClampsToZeroNearSessionStart() {
    CC_CASE("MatchInputBuffer: writeHead が offset 未満なら readPos=0");
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(3, 2, 4); // offset=6 > wh=3

    CC_CHECK_EQ(b.GetReadPos(), 0u);
}

// ============================================================================
// MatchInputBuffer — 読み出しの安全性
// ============================================================================

static void Read_FrameZeroIsReadableWhenConfirmed() {
    CC_CASE("MatchInputBuffer: フレーム0も確定していれば読み出せる");
    // 旧実装は readPos==0 を「無効」と扱い、フレーム0を永久に配れなかった。
    // valid フラグで「未書込み」と「フレーム0」を区別する。
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(1, 0, 0); // offset=1 → readPos = 0
    b.WriteLocal(0, 0x1111, 0, false);
    b.ConfirmRemote(0, 0x2222);
    b.SetWriteHead(1);

    uint32_t p1 = 0, p2 = 0;
    CC_CHECK(b.TryReadForGame(true, p1, p2));
    CC_CHECK_EQ(p1, 0x1111u);
    CC_CHECK_EQ(p2, 0x2222u);
}

static void Read_RejectsUnwrittenSlot() {
    CC_CASE("MatchInputBuffer: 未書込みスロットは読み出さない");
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(100, 2, 4); // readPos=94, 未書込み

    uint32_t p1 = 0xDEAD, p2 = 0xBEEF;
    CC_CHECK(!b.TryReadForGame(true, p1, p2));
    CC_CHECK_EQ(p1, 0xDEADu); // 出力は変更されない
}

static void Read_RejectsUnconfirmedSlot() {
    CC_CASE("MatchInputBuffer: 相手入力が未確定なら読み出さない");
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(100, 2, 4);
    b.WriteLocal(94, 0xAAAA, 0xBBBB, false); // 予測のみ
    b.SetWriteHead(100);

    uint32_t p1 = 0, p2 = 0;
    CC_CHECK(!b.TryReadForGame(true, p1, p2));
}

static void Read_SwapsSidesByHostRole() {
    CC_CASE("MatchInputBuffer: isHost で P1/P2 が入れ替わる");
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(100, 2, 4); // readPos = 94
    b.WriteLocal(94, 0xAAAA, 0, false);
    b.ConfirmRemote(94, 0xBBBB);
    b.SetWriteHead(100);

    uint32_t p1 = 0, p2 = 0;
    CC_CHECK(b.TryReadForGame(/*isHost*/ true, p1, p2));
    CC_CHECK_EQ(p1, 0xAAAAu);
    CC_CHECK_EQ(p2, 0xBBBBu);

    CC_CHECK(b.TryReadForGame(/*isHost*/ false, p1, p2));
    CC_CHECK_EQ(p1, 0xBBBBu);
    CC_CHECK_EQ(p2, 0xAAAAu);
}

static void Read_RejectsWrappedSlot() {
    CC_CASE("MatchInputBuffer: リング周回で別フレームになったスロットを拒否する");
    // 旧実装はフレーム番号を検証せず、周回後に別フレームのデータを黙って返した。
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(0, 0, 0);

    b.WriteLocal(10, 0x1111, 0, false);
    b.ConfirmRemote(10, 0x2222);
    // 同じスロットを 600F 先のフレームで踏み潰す
    b.WriteLocal(10 + MatchInputBuffer::RING_SIZE, 0x3333, 0, false);

    CC_CHECK(!b.HasLocal(10)); // フレーム10 はもう存在しない
    CC_CHECK(b.HasLocal(610));

    b.SetWriteHead(11); // readPos = 10
    uint32_t p1 = 0xDEAD, p2 = 0xBEEF;
    CC_CHECK(!b.TryReadForGame(true, p1, p2)); // 610 のデータを返さない
    CC_CHECK_EQ(p1, 0xDEADu);
}

// ============================================================================
// MatchInputBuffer — 確定とミスマッチ検出
// ============================================================================

static void Confirm_SetsFrameEvenWithoutLocalWrite() {
    CC_CASE("MatchInputBuffer: 自入力より先に相手入力が来ても frame が立つ");
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(0, 2, 4);

    b.ConfirmRemote(50, 0x99);

    uint32_t remote = 0;
    CC_CHECK(b.TryGetRemoteInput(50, remote));
    CC_CHECK_EQ(remote, 0x99u);
    CC_CHECK(b.HasRemote(50));
    CC_CHECK(!b.HasLocal(50)); // 自入力はまだ来ていない
}

static void Confirm_PreservesRemoteWhenLocalArrivesLater() {
    CC_CASE("MatchInputBuffer: 確定済みフレームに自入力が来ても確定値を壊さない");
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(0, 2, 4);

    b.ConfirmRemote(50, 0x99);
    b.WriteLocal(50, 0xAA, /*predicted*/ 0x00, false);

    uint32_t local = 0, remote = 0;
    CC_CHECK(b.TryGetLocalInput(50, local));
    CC_CHECK(b.TryGetRemoteInput(50, remote));
    CC_CHECK_EQ(local, 0xAAu);
    CC_CHECK_EQ(remote, 0x99u); // 予測で上書きされない
}

static void Mismatch_DetectsWrongPrediction() {
    CC_CASE("MatchInputBuffer: 予測外れを検出する");
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(0, 2, 4);
    b.WriteLocal(20, 0, /*予測*/ 0xAA, true);

    CC_CHECK(!b.HasMismatch());
    b.ConfirmRemote(20, 0xCC);
    CC_CHECK(b.HasMismatch());

    uint32_t f = 0;
    CC_CHECK(b.ConsumeMismatch(f));
    CC_CHECK_EQ(f, 20u);
    CC_CHECK(!b.ConsumeMismatch(f)); // 消費後はクリアされる
}

static void Mismatch_KeepsOldestFrame() {
    CC_CASE("MatchInputBuffer: ミスマッチは最も古いフレームを残す");
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(0, 2, 4);
    b.WriteLocal(20, 0, 0xAA, true);
    b.WriteLocal(10, 0, 0xBB, true);

    b.ConfirmRemote(20, 0xCC);
    b.ConfirmRemote(10, 0xDD);

    uint32_t f = 0;
    CC_CHECK(b.ConsumeMismatch(f));
    CC_CHECK_EQ(f, 10u); // ロールバックの起点は古い方
}

static void Mismatch_AtFrameZeroIsDistinguishable() {
    CC_CASE("MatchInputBuffer: フレーム0のミスマッチも「なし」と区別できる");
    // 旧実装は「なし」をフレーム0で表していたため区別不能だった。
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(0, 2, 4);
    b.WriteLocal(0, 0, /*予測*/ 0xAA, true);

    b.ConfirmRemote(0, 0xBB);

    CC_CHECK(b.HasMismatch());
    uint32_t f = 0xFFFFFFFF;
    CC_CHECK(b.ConsumeMismatch(f));
    CC_CHECK_EQ(f, 0u);
}

static void Confirm_ConflictOnAlreadyConfirmedIsCounted() {
    CC_CASE("MatchInputBuffer: 確定済みフレームへの異なる再確定を数える");
    // 冗長入力は同じフレームを何度も確定する。値が食い違うのは通信破綻か
    // デシンクであり、旧実装のように黙って上書きしてはいけない。
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(0, 2, 4);
    b.WriteLocal(30, 0, 0, true);
    b.ConfirmRemote(30, 0xAA);

    CC_CHECK_EQ(b.ConfirmConflicts(), 0u);

    b.ConfirmRemote(30, 0xAA); // 同じ値の再送は正常
    CC_CHECK_EQ(b.ConfirmConflicts(), 0u);

    b.ConfirmRemote(30, 0xFF); // 食い違い
    CC_CHECK_EQ(b.ConfirmConflicts(), 1u);
    CC_CHECK(b.HasMismatch());

    uint32_t remote = 0;
    CC_CHECK(b.TryGetRemoteInput(30, remote));
    CC_CHECK_EQ(remote, 0xAAu); // 最初の確定値を正とする
}

static void Confirm_AdvancesConfirmedFrameMonotonically() {
    CC_CASE("MatchInputBuffer: confirmedRemoteFrame は後退しない");
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(0, 2, 4);

    CC_CHECK(!b.HasConfirmedRemote());
    b.ConfirmRemote(20, 0x01);
    CC_CHECK(b.HasConfirmedRemote());
    CC_CHECK_EQ(b.GetConfirmedRemoteFrame(), 20u);

    b.ConfirmRemote(10, 0x02); // 冗長入力による過去フレームの再確定
    CC_CHECK_EQ(b.GetConfirmedRemoteFrame(), 20u);
}

static void ConfirmedFrameZero_IsDistinguishableFromNone() {
    CC_CASE("MatchInputBuffer: フレーム0の確定も「未確定」と区別できる");
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(0, 2, 4);

    CC_CHECK(!b.HasConfirmedRemote());
    b.ConfirmRemote(0, 0x77);
    CC_CHECK(b.HasConfirmedRemote());
    CC_CHECK_EQ(b.GetConfirmedRemoteFrame(), 0u);
}

// ============================================================================
// MatchInputBuffer — 進行可能フレームと状態クリア
// ============================================================================

static void EffectiveHead_IsCappedByPeerConfirmation() {
    CC_CASE("MatchInputBuffer: effectiveHead は相手の確定フレームで頭打ちになる");
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(100, 2, 4); // wh=100, offset=6, 相手未確定

    CC_CHECK_EQ(b.GetEffectiveHead(), 94u); // 相手未確定なら遅延分のみ

    b.ConfirmRemote(100, 0x01);
    b.SetWriteHead(200);                     // 自分だけ先行
    CC_CHECK_EQ(b.GetEffectiveHead(), 100u); // min(194, 100) → 相手待ち
}

static void Reset_KeepsSessionParamsButClearsProgress() {
    CC_CASE("MatchInputBuffer: Reset は D/R を残し、進行状態だけ消す");
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(100, 3, 5);
    b.WriteLocal(94, 0x1234, 0, false);
    b.ConfirmRemote(94, 0x5678);

    b.Reset();

    CC_CHECK_EQ(b.GetDelay(), 3);
    CC_CHECK_EQ(b.GetMaxRollback(), 5);
    CC_CHECK_EQ(b.GetWriteHead(), 0u);
    CC_CHECK(!b.HasConfirmedRemote());
    CC_CHECK(!b.HasMismatch());
    CC_CHECK_EQ(b.ConfirmConflicts(), 0u);
    CC_CHECK(!b.HasLocal(94));
    CC_CHECK(!b.HasRemote(94));
}

static void Singleton_SharesStateAcrossCallSites() {
    CC_CASE("MatchInputBuffer: シングルトンなので状態がフェーズ間で残る");
    // Reset() を呼ぶ責任は呼び出し側にある。
    auto &a = MatchInputBuffer::GetInstance();
    a.Initialize(100, 2, 4);
    a.WriteLocal(94, 0x1234, 0, false);

    auto &b = MatchInputBuffer::GetInstance();
    CC_CHECK(&a == &b);
    CC_CHECK(b.HasLocal(94));
}

// ============================================================================
// レーン分離（2スレッド競合の解消）
// ============================================================================

static void Lanes_LocalAndRemoteArePublishedIndependently() {
    CC_CASE("MatchInputBuffer: local と remote は独立に公開される");
    // ゲームスレッドは local レーン、通信スレッドは remote レーンだけを書く。
    // 片方だけ揃っている状態が正しく区別できることを固定する。
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(0, 2, 4);

    uint32_t v = 0;
    CC_CHECK(!b.HasLocal(40));
    CC_CHECK(!b.HasRemote(40));

    b.WriteLocal(40, 0x11, 0x00, false);
    CC_CHECK(b.HasLocal(40));
    CC_CHECK(!b.HasRemote(40));
    CC_CHECK(b.TryGetLocalInput(40, v));
    CC_CHECK_EQ(v, 0x11u);
    CC_CHECK(!b.TryGetRemoteInput(40, v));

    b.ConfirmRemote(40, 0x22);
    CC_CHECK(b.HasRemote(40));
    CC_CHECK(b.TryGetRemoteInput(40, v));
    CC_CHECK_EQ(v, 0x22u);
}

static void Lanes_WriteLocalDoesNotClearConfirmedRemote() {
    CC_CASE("MatchInputBuffer: 自入力の書込みが相手の確定を消さない");
    // 旧実装は WriteLocal が remoteInput/confirmed を触っていたため、
    // 確定済みの相手入力を予測値で潰す競合があった。
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(0, 2, 4);

    b.ConfirmRemote(60, 0xBEEF);
    b.WriteLocal(60, 0xCAFE, /*predicted*/ 0x0000, false);

    uint32_t remote = 0;
    CC_CHECK(b.TryGetRemoteInput(60, remote));
    CC_CHECK_EQ(remote, 0xBEEFu);
}

static void Lanes_EmptySlotIsDistinguishableFromFrameZero() {
    CC_CASE("MatchInputBuffer: 未使用スロットとフレーム0を取り違えない");
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(0, 2, 4);

    CC_CHECK(!b.HasLocal(0)); // Reset 直後は未使用
    b.WriteLocal(0, 0x55, 0, false);
    CC_CHECK(b.HasLocal(0)); // フレーム0 を書けば有効になる
}

// ============================================================================

int main() {
    ReadPos_SubtractsDelayPlusRollback();
    ReadPos_ClampsOffsetToAtLeastOne();
    ReadPos_ClampsToZeroNearSessionStart();

    Read_FrameZeroIsReadableWhenConfirmed();
    Read_RejectsUnwrittenSlot();
    Read_RejectsUnconfirmedSlot();
    Read_SwapsSidesByHostRole();
    Read_RejectsWrappedSlot();

    Confirm_SetsFrameEvenWithoutLocalWrite();
    Confirm_PreservesRemoteWhenLocalArrivesLater();
    Mismatch_DetectsWrongPrediction();
    Mismatch_KeepsOldestFrame();
    Mismatch_AtFrameZeroIsDistinguishable();
    Confirm_ConflictOnAlreadyConfirmedIsCounted();
    Confirm_AdvancesConfirmedFrameMonotonically();
    ConfirmedFrameZero_IsDistinguishableFromNone();

    EffectiveHead_IsCappedByPeerConfirmation();
    Reset_KeepsSessionParamsButClearsProgress();
    Singleton_SharesStateAcrossCallSites();

    Lanes_LocalAndRemoteArePublishedIndependently();
    Lanes_WriteLocalDoesNotClearConfirmedRemote();
    Lanes_EmptySlotIsDistinguishableFromFrameZero();

    return cccaster::test::Summarize("input_buffers");
}
