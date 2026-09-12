# DummyPeer v2 要件定義書 — 03. 実装計画書

> **文書ID**: REQ-DP2-003  
> **作成日**: 2026-02-27  
> **ステータス**: レビュー待ち

---

## 1. 実装フェーズ

| フェーズ | 内容 | 変更ファイル数 | 依存関係 |
|---|---|---|---|
| **A** | WASAPI対応ダミーアプリ基盤 | 3ファイル (MODIFY) | なし |
| **B** | パケット仕様拡張 | 2ファイル (MODIFY) | なし |
| **C** | 疑似画面遷移エンジン | 5ファイル (NEW) + 2ファイル (MODIFY) | A, B |
| **D** | コントローラ入力シミュレーション | 1ファイル (NEW) | なし |
| **E** | テストモード統合 | 3ファイル (NEW) + 2ファイル (MODIFY) | A, B, C, D |

---

## 2. フェーズ A: WASAPI対応ダミーアプリ基盤

### 2.1 問題

現行DummyPeerはコンソールアプリ (`main()`) のため、WASAPIの `CoCreateInstance(CLSID_MMDeviceEnumerator)` が失敗し、QPCフォールバックになる。本番DLLはゲームプロセス内（GUI環境）で実行されるためWASAPIが正常に動作する。

### 2.2 解決策

`main()` → `WinMain()` に変更し、**非表示ウィンドウ（Hidden Window）**を持つWin32アプリとして構築する。

### 2.3 ファイル変更

#### [MODIFY] `src/main.cpp`

```cpp
// 変更前:
int main(int argc, char* argv[]) {

// 変更後:
#include <windows.h>
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // コンソール出力の維持
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
    }
    // stdout/stderrを再接続
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);

    // Hidden Window 作成（WASAPI/COM の安定動作用）
    WNDCLASS wc = {};
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "DummyPeerHiddenWindow";
    RegisterClass(&wc);
    HWND hwnd = CreateWindow(wc.lpszClassName, "", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, NULL, hInstance, NULL);

    // 既存のCLI引数はそのまま利用
    int argc = __argc;
    char** argv = __argv;
    // ... 以降は既存ロジック ...
}
```

#### [MODIFY] `src/SyncResponder.cpp`

```diff
 void SyncResponder::InitWasapi() {
-    CoInitializeEx(NULL, COINIT_MULTITHREADED);
+    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
```

#### [MODIFY] `CMakeLists.txt`

```diff
-add_executable(DummyPeer
+add_executable(DummyPeer WIN32
     src/main.cpp
     ...
 )
 
-target_link_libraries(DummyPeer PRIVATE ws2_32 winmm ole32 uuid)
+target_link_libraries(DummyPeer PRIVATE ws2_32 winmm ole32 uuid avrt)
```

---

## 3. フェーズ B: パケット仕様拡張

### 3.1 ファイル変更

#### [MODIFY] `include/UnifiedProtocol.hpp`

追加内容:
- `PeerState` enum class (8ステート)
- `UnifiedPacketHeader` を 16B → 20B に拡張 (`peerState` + `reserved[3]`)
- `PacketType` に `LOADING_INPUT=0x20`, `REMATCH_MENU=0x50`, `STATE_REPORT=0xE0` 追加
- `LoadingInputPayload`, `RematchMenuPayload`, `StateReportPayload` 構造体追加
- `MakeHeader()` に `peerState` 引数追加
- `BuildPacket()` テンプレートの `MakeHeader()` 呼び出し更新
- `ParseHeader()` の `len` チェックを 16B 互換対応

#### [MODIFY] `include/PhaseHandler.hpp` / `src/PhaseHandler.cpp`

追加内容:
- `HandleLoadingInput()`, `HandleRematchMenu()`, `HandleStateReport()` ハンドラ
- `HandlePacket()` の `switch` に新タイプ追加
- `GeneratePacket()` に `LOADING`, `REMATCH` フェーズ対応

---

## 4. フェーズ C: 疑似画面遷移エンジン

### 4.1 新規ファイル一覧

| ファイル | サイズ目安 | 責務 |
|---|---|---|
| `include/SceneStateMachine.hpp` | ~80行 | PeerState管理、遷移ルール |
| `include/DummySceneCharaSelect.hpp` | ~120行 | BOOTING～CS_STAGE_SELECT の5ステート |
| `include/DummySceneLoading.hpp` | ~80行 | LOADING の3フェーズ |
| `include/DummySceneInGame.hpp` | ~100行 | IN_GAME (intro同期+GAME_INPUT) |
| `include/DummySceneRematch.hpp` | ~70行 | REMATCH (ゲート方式) |

### 4.2 SceneStateMachine 設計

```cpp
class SceneStateMachine {
public:
    using SendFunc = std::function<void(const std::vector<uint8_t>&)>;
    
    explicit SceneStateMachine(const Config& config, bool isHost,
                               SyncResponder& syncResp);
    
    // 毎フレーム呼び出し（16ms周期）
    void Update(SendFunc send);
    
    // パケット受信処理
    void HandlePacket(const uint8_t* data, int len, SendFunc send);
    
    // 状態アクセス
    PeerState GetState() const { return _state; }
    PeerState GetRemoteState() const { return _remoteState; }
    uint32_t GetFramesInState() const { return _framesInState; }
    
    // 状態遷移
    void TransitionTo(PeerState newState);
    
    // レポート
    void PrintReport() const;
    
private:
    PeerState _state = PeerState::BOOTING;
    PeerState _remoteState = PeerState::BOOTING;
    uint32_t _framesInState = 0;
    
    Config _config;
    bool _isHost;
    SyncResponder& _syncResp;
    
    // 各Scene
    DummySceneCharaSelect _sceneCS;
    DummySceneLoading     _sceneLoading;
    DummySceneInGame      _sceneInGame;
    DummySceneRematch     _sceneRematch;
};
```

### 4.3 各DummyScene の処理するPeerState範囲

| DummyScene | PeerState | 本番処理との対応 |
|---|---|---|
| `DummySceneCharaSelect` | BOOTING | 50Fカウント→CS_SYNC_WAITへ遷移 |
| | CS_SYNC_WAIT | SYNC_REQ/RES交換→SYNC_DONE送受信 |
| | CS_SYNC_DONE | ClockOffset適用（即座にCS_SELECTINGへ） |
| | CS_SELECTING | キャラ入力(CS_INPUT)送信(3種フィルタ付き) |
| | CS_STAGE_SELECT | ステージ確定→LOADINGへ遷移 |
| `DummySceneLoading` | LOADING | Phase1:30F待ち→Phase2:TimeSync→Phase3:ディレイ入力 |
| `DummySceneInGame` | IN_GAME | intro同期→RE初期化→GAME_INPUT 60fps送信 |
| `DummySceneRematch` | REMATCH | 待ちフレーム→REMATCH_MENU送受信→遷移先決定 |

### 4.4 DummySceneCharaSelect — サブステート詳細

```
BOOTING (50F)
  └─ framesInState >= 50 → CS_SYNC_WAIT

CS_SYNC_WAIT
  ├─ CS_SYNC_READY パケット送信
  ├─ SYNC_REQ 受信 → SYNC_RES 返信
  ├─ SYNC_RES 受信 → θ算出
  ├─ 10往復完了 & SYNC_DONE 交換
  └─ → CS_SYNC_DONE

CS_SYNC_DONE
  ├─ ClockOffset 適用
  └─ 即座に → CS_SELECTING

CS_SELECTING
  ├─ CS_INPUT パケット送受信 (60fps)
  ├─ FilterA: 150F間 確定ボタン無効
  ├─ FilterB: キャラ選択中はキャンセル無効
  ├─ キャラ確定 → ムーン確定 → カラー確定
  └─ 全確定 → CS_STAGE_SELECT

CS_STAGE_SELECT
  ├─ ステージ選択入力送信
  └─ 確定 → LOADING
```

### 4.5 DummySceneLoading — 3フェーズ詳細

```
Phase 1: 安定待ち (30F)
  └─ 入力クリア + SleepFrame

Phase 2: 時刻同期
  ├─ SYNC_REQ/RES 交換 (TimeSynchronizer互換)
  ├─ SYNC_DONE 交換
  └─ ApplySyncOffset

Phase 3: ディレイ入力交換
  ├─ delay = ceil((RTT/2 + 1F) / 16.67ms), min=2, max=15
  ├─ LOADING_INPUT パケット送受信
  └─ バッファ溜め完了 → IN_GAME
```

### 4.6 DummySceneInGame — 対戦処理

```
intro=2 同期:
  ├─ PauseForSync
  ├─ SYNC_REQ/RES 交換
  ├─ SYNC_DONE 交換 + WaitUntilStartTime
  └─ RE初期化  

対戦ループ (60fps):
  ├─ GAME_INPUT (11F冗長化) 送信
  ├─ GAME_INPUT 受信 + roundTimer 監視
  └─ 設定ラウンド数消化 → REMATCH
```

### 4.7 DummySceneRematch — メニュー同期

```
待機 (設定フレーム数):
  └─ STATE_REPORT(REMATCH) 定期送信

メニュー選択:
  ├─ REMATCH_MENU(menuIndex, confirmed=1) 送信
  ├─ 相手の REMATCH_MENU 受信
  └─ max(local, remote) で遷移先決定

遷移:
  ├─ menuIndex==0 (Rematch) → LOADING
  └─ menuIndex==1 (CharaSelect) → CS_SELECTING
```

---

## 5. フェーズ D: コントローラ入力シミュレーション

#### [NEW] `include/ControllerInputSim.hpp`

```cpp
class ControllerInputSim {
public:
    // MBAA ボタン定数 (CC_BUTTON_* 互換)
    static constexpr uint16_t BTN_A       = 0x0001;
    static constexpr uint16_t BTN_B       = 0x0002;
    static constexpr uint16_t BTN_C       = 0x0004;
    static constexpr uint16_t BTN_D       = 0x0008;
    static constexpr uint16_t BTN_E       = 0x0010;
    static constexpr uint16_t BTN_AB      = 0x0020;
    static constexpr uint16_t BTN_START   = 0x0040;
    static constexpr uint16_t BTN_CONFIRM = 0x0200;
    static constexpr uint16_t BTN_CANCEL  = 0x0400;
    
    // キャラセレ: フレーム番号に応じたリアルな選択シーケンス
    // 0-30F: ニュートラル → 31-60F: カーソル移動 → 61F: 確定 → ...
    uint16_t GenerateCharaSelectInput(uint32_t frameInPhase);
    
    // Loading: ニュートラル（ロード中は入力不要）
    uint16_t GenerateLoadingInput();
    
    // InGame: ランダム戦闘入力（方向+ボタン組合せ）
    // 本番SceneRunnerのGenerateRandomTestInput()と同等ロジック
    uint32_t GenerateGameInput();
    
    // Rematch: メニュー選択 (設定に基づく)
    int8_t GenerateRematchSelection(int rematchChoice);
};
```

---

## 6. フェーズ E: テストモード統合

### 6.1 TestMode 拡張

```cpp
enum class TestMode {
    Negotiation,  // 既存: 接続確立のみ
    Sync,         // 既存: 時刻同期+入力テスト
    Full,         // 既存: Phase 1.5→2→4 フルサイクル
    Game,         // 既存: リアクティブ方式（完全後方互換）
    E2E,          // [NEW] 全画面遷移の本番フロー再現テスト
};
```

### 6.2 Config 追加フィールド

```cpp
// E2E テスト設定
int roundFrames = 3600;              // 1ラウンドのフレーム数（60秒）
int maxRounds = 2;                   // ラウンド数
int rematchChoice = 0;               // 0=Rematch, 1=CharaSelect
int rematchWaitFrames = 180;         // リマッチ画面の待ちフレーム
bool autoRematch = true;             // 自動リマッチ（ループテスト用）
int charaSelectFrames = 300;         // キャラセレ選択にかけるフレーム数
int stageSelectFrames = 60;          // ステージ選択にかけるフレーム数
```

### 6.3 CLI 引数追加

```
--test-mode e2e           E2Eテストモード
--round-frames <n>        1ラウンドのフレーム数 (default: 3600)
--max-rounds <n>          ラウンド数 (default: 2)
--rematch-choice <n>      0=Rematch, 1=CharaSelect (default: 0)
--auto-rematch            自動リマッチ有効 (default: true)
--chara-select-frames <n> キャラセレフレーム数 (default: 300)
```

### 6.4 新規ファイル

#### [NEW] `include/E2ETest.hpp` / `src/E2ETest.cpp`

```cpp
class E2ETest : public TestRunner {
public:
    TestResult Run(SOCKET sock, const Config& config,
                   std::atomic<bool>& running) override;
private:
    SceneStateMachine _stateMachine;
    // Negotiation → SceneStateMachine 駆動ループ
};
```

---

## 7. ファイル変更一覧（完全版）

| フェーズ | 操作 | ファイル | 行数目安 |
|---|---|---|---|
| A | MODIFY | `src/main.cpp` | +30行 |
| A | MODIFY | `src/SyncResponder.cpp` | +2行 |
| A | MODIFY | `CMakeLists.txt` | +3行 |
| B | MODIFY | `include/UnifiedProtocol.hpp` | +60行 |
| B | MODIFY | `include/PhaseHandler.hpp` + `src/PhaseHandler.cpp` | +80行 |
| C | NEW | `include/SceneStateMachine.hpp` | ~80行 |
| C | NEW | `include/DummySceneCharaSelect.hpp` | ~120行 |
| C | NEW | `include/DummySceneLoading.hpp` | ~80行 |
| C | NEW | `include/DummySceneInGame.hpp` | ~100行 |
| C | NEW | `include/DummySceneRematch.hpp` | ~70行 |
| D | NEW | `include/ControllerInputSim.hpp` | ~80行 |
| E | MODIFY | `include/DummyPeer.hpp` | +20行 |
| E | MODIFY | `src/DummyPeer.cpp` | +10行 |
| E | NEW | `include/E2ETest.hpp` | ~40行 |
| E | NEW | `src/E2ETest.cpp` | ~200行 |

**合計**: 8ファイル変更 + 8ファイル新規作成 ≈ **+975行**

---

## 8. 検証計画

### 8.1 自動テスト

| # | テスト | コマンド | 成功判定 |
|---|---|---|---|
| 1 | ビルド | `cmake --build build -j8` | DummyPeer.exe 生成 |
| 2 | WASAPI | `DummyPeer.exe --test-mode sync --duration 3` | `WASAPI: OK` |
| 3 | 後方互換1 | `DummyPeer.exe --test-mode negotiation --duration 5` | クラッシュなし |
| 4 | 後方互換2 | `DummyPeer.exe --test-mode sync --duration 5` | クラッシュなし |
| 5 | 後方互換3 | `DummyPeer.exe --test-mode full --duration 5` | クラッシュなし |
| 6 | 後方互換4 | `DummyPeer.exe --test-mode game --duration 5` | クラッシュなし |
| 7 | E2Eテスト | `DummyPeer.exe --test-mode e2e --duration 30` | 全PeerState遷移完了 |
| 8 | JSON出力 | `DummyPeer.exe --test-mode e2e --output-json e2e.json` | JSONファイル生成 |

### 8.2 手動検証（マスター確認事項）

1. DummyPeer起動時のログで `[SyncResponder] WASAPI: OK` を確認
2. `--test-mode game` で CCCaster_v10.exe + DummyPeer を同時起動し通信動作を確認
3. E2Eログで全PeerState遷移（BOOTING→CS_SYNC_WAIT→...→REMATCH→CS_SELECTING）の完了を確認

---

## 9. リスクと対策

| リスク | 影響度 | 対策 |
|---|---|---|
| WASAPI が Hidden Window でも初期化失敗 | 高 | QPCフォールバックを維持、エラーログ強化 |
| パケットヘッダ 20B化で DLL側が拒否 | 中 | 後方互換パース（16B受理→peerState=0）を実装 |
| E2Eテストで状態遷移のタイミングが本番と乖離 | 低 | Config パラメータで各フレーム数を調整可能に |
