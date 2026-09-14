# feat(DummyPeer): --test-mode e2e を実装 (DummyPeer.cpp E2Eケース追加)

## 変更内容

### tools/dummy_peer/src/DummyPeer.cpp

#### 追加: E2E テストモードの switch ケース
- `TestMode::E2E` の `case` を `Run()` の switch 文に追加
- `SyncResponder` と `NegotiationResponder` を `shared_ptr` で生成し
  `E2ETest(isHost, config, *syncResp, *negoResp)` に参照渡し
- `Run()` はブロッキングのため `shared_ptr` のライフタイムはスコープ内で安全

#### 追加: testModeStr ラムダに E2E 表示
- `case TestMode::E2E: return "E2E";` を追加（コンフィグ表示で "UNKNOWN" にならない）

#### 追加: E2E 固有パラメータのコンフィグ表示
- `[E2E] RoundFrames / MaxRounds / RematchChoice / CharaSelectFrames` を表示

#### 追加: include
- `E2ETest.hpp`, `SyncResponder.hpp`, `NegotiationResponder.hpp` を追加

## 動作確認

```
DummyPeer.exe --test-mode e2e --duration 5
```

出力例:
```
=== DummyPeer Configuration ===
  Mode:         HOST
  Test Mode:    E2E
  ...
[E2ETest] Start. isHost=1
[CharaSelect] BOOTING done (50F) → CS_SYNC_WAIT
[SSM] Transition: 0x1 -> 0x21 (frames=50)
[E2ETest] Duration expired.
[E2ETest] Done. syncCompleted=0 packets=26
[DummyPeer v2] PASS
```

クラッシュなし・`PASS` を確認。
WASAPI 初期化失敗時は QPC フォールバック動作を確認。

## 制約
- `include/` には一切変更なし
