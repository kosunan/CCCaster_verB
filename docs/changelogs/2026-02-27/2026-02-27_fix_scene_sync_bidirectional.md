feat(sync): Phase 4 シーン同期安定化 — CharaSelect/Loading/Rematch 双方向入力同期 + フィルタC実装

## feat(sync): Phase 4 シーン同期安定化 — CharaSelect/Loading/Rematch 双方向入力同期 + フィルタC実装

**日時**: 2026-02-27  
**作業者**: AI 作業者C（ロールバックエンジン・シーン同期担当）  
**ビルド確認**: ✅ Exit code: 0（エラーなし）

---

## 変更概要

05_risk_analysis.md のリスク#5（3画面の送受信TODO状態）を解消し、  
キャラセレクト・ロード・リマッチ画面の双方向入力同期を実装した。

---

## 変更ファイル一覧

### 新規インフラ（送信基盤）

| ファイル | 変更内容 |
|---|---|
| `src/domain/session/SceneRunner.hpp` | `SendFunc` 型エイリアスと `Run()` オプション引数を追加 |
| `src/domain/session/SceneRunner.cpp` | `static s_send` 追加・各Sceneディスパッチに SendFunc 伝播 |
| `src/core/hooks/GameHooks.hpp` | `GetSendFunc()` メソッドを追加（UdpSocket::Send ラムダを生成） |
| `src/dll/dllmain.cpp` | `SceneRunner::Run(ctx, sendFunc)` に変更 |

### キャラセレクト画面

| ファイル | 変更内容 |
|---|---|
| `src/domain/scene/SceneCharaSelect.hpp` | `SendFunc` 引数追加・`SetRemoteInput(uint16_t)` API 追加 |
| `src/domain/scene/SceneCharaSelect.cpp` | L200 TODO 解消・`s_remoteCharaInput` (atomic) 受信・CS_INPUT(0x20) 毎フレーム送信・**フィルタC（3Fバッファガード）実装** |

### ロード画面

| ファイル | 変更内容 |
|---|---|
| `src/domain/scene/SceneLoading.hpp` | `SendFunc` 引数追加・`SetRemoteLoadingInput(uint16_t)` API 追加 |
| `src/domain/scene/SceneLoading.cpp` | L125-126 TODO 解消・`s_remoteInput` (atomic) 受信・LOADING_INPUT(0x21) Phase3 毎フレーム送信 |

### リマッチ画面

| ファイル | 変更内容 |
|---|---|
| `src/domain/scene/SceneRematch.hpp` | `SendFunc` 引数追加 |
| `src/domain/scene/SceneRematch.cpp` | L279,284 TODO 解消・`s_remoteRetryMenuIndex` (atomic) 受信・REMATCH_MENU(0x22) 確定時1度送信・**タイムアウト検出（1800F=30秒）追加** |

### 通信インフラ

| ファイル | 変更内容 |
|---|---|
| `src/core/network/PacketRouter.cpp` | 3バイトシーン同期パケット（0x20/0x21/0x22）のルーティング追加（後方互換維持） |
| `src/domain/sync/InputFilter.cpp` | フィルタA/B整備・フィルタCは SceneCharaSelect.cpp 内実装である旨のコメント追加 |

---

## パケット種別定義

| Type | 名前 | 形式 | 方向 |
|---|---|---|---|
| `0x20` | CS_INPUT | `[0x20][input_lo][input_hi]` | 毎フレーム双方向 |
| `0x21` | LOADING_INPUT | `[0x21][input_lo][input_hi]` | Phase3 毎フレーム双方向 |
| `0x22` | REMATCH_MENU | `[0x22][menu_index][0x00]` | 確定時1度のみ双方向 |

---

## リスク解消状況

| リスク# | 内容 | 解消状況 |
|---|---|---|
| #5 | 3画面の送受信がTODO状態 | ✅ 解消 |
| #9 | TimeHooks 1000倍速のTimeSync干渉 | ✅ 確認済み（RealQPC接続済み、変更不要） |

---

## 今後の課題（本コミットスコープ外）

- `SceneRematch` のタイムアウト終了処理（現状はログのみ）。断線通知 API の実装が必要
- `SceneCharaSelect` Phase 1.5 PCスペックベンチマーク（TODO として保留）
- `historyInputs` パケロス復旧ロジック（リスク#8）
