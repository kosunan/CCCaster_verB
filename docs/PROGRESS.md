# CCCaster_v10 開発進捗管理

最終更新: 2026-02-27

---

## フェーズ進捗

| Phase | 内容 | 状態 |
|-------|------|------|
| Phase 1 | CLI画面処理 | ✅ 完了 |
| Phase 2 | UDP通信の確立 | ✅ 完了 |
| Phase 3 | オフライントレーニングモード起動確認 | ✅ 完了 |
| Phase 4-1 | キャラクターセレクトの同期 | 🟡 E2Eテスト中（LOADING遷移まで成功，バグ修正作業中） |
| Phase 4-2 | オフライン(Training)完全同期 | ⬜ 未着手 |
| Phase 4-3 | InGame UI (ImGui オーバーレイ) | ⬜ 未着手 |
| Phase 4-4 | ラウンド開始時同期 | ⬜ 未着手 |
| Phase 4-5 | 高精度ロールバック同期 | ⬜ 未着手 |

---

## 直近の実施内容（2026-02-27）

| コミット | 内容 | 担当 |
|----------|------|------|
| `bc212d8` | PacketRouter コメント整備・RedundantProtocol `[DEAD CODE]` 明記 | 作業A |
| `1ff6674` | 04_packet_analysis_report.md 更新（乖離5件・ルーティング追記） | 作業A |
| `de2f0e9` | Scene全送受信実装・InputFilter フィルタC・PacketRouter 3Bルーティング | 作業C |
| `1d94f69` | ControllerMapper 7メソッド分割・CLI Rollback設定自動化 | 作業B |
| `06a8b15` | changelog補完 | 作業B |
| `0ea2d64` | PacketRouter 統一ヘッダ(20B/CC10) 5段階ディスパッチ対応 | 作業B2 |
| `8a4103b` | DummyPeer v2 完全再構築（WinMain化・疑似画面遷移・E2Eテスト基盤） | 作業C2 |
| `fab0556` | DummyPeer.cpp に TestMode::E2E 分岐追加・単独起動PASS確認 | 作業A |
| `c961557` | DummyPeer GameTest 旧3Bシーン同期パケット受信対応Ｈ0x20/0x21/0x22） | 作業B |

---

## 検証結果（2026-02-27）

### Negotiationテスト ✅ PASS

```
Connected: Yes | Ping: 330ms | Jitter: 5.13ms | Lock-in: SUCCESS
```

**確定した正確な起動引数形式:**

```powershell
# Host側 (CCCaster_v10小_TEST_MBAACC下から起動)
Set-Location "I:\work_space\CCCaster_v10\_TEST_MBAACC\cccaster"
.\CCCaster_B.exe --headless --host --port 10800

# DummyPeer 側 (projectルートから)
.\tools\dummy_peer\build\DummyPeer.exe `
    --mode client `
    --test-mode negotiation `
    --target-ip 127.0.0.1 `
    --local-port 10801 `
    --remote-port 10800 `
    --duration 15
```

### E2Eテスト — 次のマイルストーン (未実施)

```powershell
.\tools\dummy_peer\build\DummyPeer.exe `
    --mode client `
    --test-mode e2e `
    --target-ip 127.0.0.1 `
    --local-port 10801 `
    --remote-port 10800 `
    --duration 60
```

### E2Eテスト — 成功ログ（2026-02-27）

```
[CharaSelect] BOOTING done (50F)         → CS_SYNC_WAIT   ✅
[CharaSelect] TimeSync done (rounds=10)  → CS_SYNC_DONE   ✅
[CharaSelect] CS_SYNC_DONE: offset=0    → CS_SELECTING   ✅
[CharaSelect] CS_SELECTING done (300F)  → CS_STAGE_SELECT ✅
[CharaSelect] Stage selected             → LOADING        ✅
[E2ETest] Duration expired. (LOADING内で時間切れ)
[DummyPeer v2] PASS
```

**未達成**（Duration切れ）: IN_GAME, REMATCH → 次フェーズで検証

**発見バグ** → ISSUE_TRACKER.md E-5/E-6/E-7 参照

---

## 残課題・次回以降のタスク

| 優先度 | 内容 | 関連 |
|--------|------|------|
| 🔴 高 | **E2Eテスト**（DummyPeer --test-mode e2e で DLLと全PeerState遷移確認） | Phase 4-1 検証 |
| 🔴 高 | SceneRematch タイムアウト時の実切断処理（現状ログのみ） | Phase 4-1 安定性 |
| 🔴 高 | SceneRematch タイムアウト時の実切断処理（現状ログのみ） | Phase 4-1 安定性 |
| 🟡 中 | リスク#8 `historyInputs` パケロス復旧ロジック | Phase 4-2 |
| 🟡 中 | RedundantProtocol を仕様書準拠(CC10/20B)で再設計（GAME_INPUT実装時） | Phase 4-2 |
| 🟢 低 | Phase 1.5 PCスペックベンチマーク | 将来 |

---

## アーキテクチャ決定記録 (ADR)

| 日付 | 決定事項 |
|------|----------|
| 2026-02-27 | PacketRouter統一ヘッダ対応はDomain層入力送信実装と同時に行う（先行スケルトン不要） → B2で先行実装済みに変更 |
| 2026-02-27 | RedundantProtocol は削除せず `[DEAD CODE]` 保留。再設計は Phase 4-2 GAME_INPUT 時 |
| 2026-02-27 | シーン→UDP送信は `SceneRunner::Run()` に `SendFunc` ラムダで差し込む（案A） |
| 2026-02-27 | PacketRouter 5段階ディスパッチ: CC10マジック先行 → SYNC旧型 → 3B後方互換 → 2B後方互換 → UNKNOWN |
| 2026-02-27 | CLI設定デフォルト値: Delay=2, Rollback=4。INIパスは `cccaster\cccaster_v10.ini` 固定（未存在時は警告のみ） |
| 2026-02-27 | DummyPeer v2: WinMain化・Hidden Window・20Bヘッダ・SceneStateMachine 疑似画面遷移方式を採用 |
