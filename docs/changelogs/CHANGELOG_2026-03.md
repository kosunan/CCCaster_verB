# refactor: スレッドモデル再設計 — DLLスレッド駆動 + 通信スレッド送信専念化

## 2026-03-08

### Changed — スレッドモデル (Step 1-3)

| ファイル | 変更内容 |
|----------|----------|
| `CentralBuffer.hpp` | `CommitFrame()` 引数なし版追加（内部で Phase取得・入力Poll・rollbackable判定を自前収集）。`_isHost`メンバ追加。`Initialize(isHost)` 引数追加 |
| `SceneRunner.cpp` | `Step()` をメトロノーム駆動に変更: `ConsumeTicks→CommitFrame→ReadFrameForGame→WriteInput` |
| `SyncCoordinator.cpp` | Counting モードを送信専念化: CB writeHead 監視→パケット送信のみ。入力読取・AdvanceFrame・キャッチアップバースト削除 |
| `SyncCoordinator.hpp` | `GetMetronome()` アクセサ追加。`_lastLocalInput` → `_lastSentFrame`/`_lastLogFrame` に変更 |

---

# refactor: CentralBuffer 管理ルール整理 + 散発修正

## 2026-03-06 (2)

### Changed — CentralBuffer 統合API

| ファイル | 変更内容 |
|----------|---------|
| `CentralBuffer.hpp` | `CommitFrame()`, `ReadFrameForGame()`, `Initialize()` 新設。全メソッドに `@thread_safety` コメント追加。Read API 使い分けガイド追加。 |
| `SyncCalculator.cpp` | `WriteSlot()+SetWriteHead()` → `CommitFrame()` に統合 |
| `SceneBusiness.cpp` | `GetReadPos()+GetSlot()+P1/P2振分け` → `ReadFrameForGame()` に統合 |
| `SyncCoordinator.cpp` | 4連続CentralBuffer呼出し → `Initialize()` に統合 |

### Fixed — 散発修正

| ファイル | 変更内容 |
|----------|---------|
| `SessionNegotiator.hpp` | 未使用 `UdpSocket.hpp` include 削除 |
| `SessionNegotiator.cpp` | `UdpSocket.hpp` include を `.cpp` に移動、`<iomanip>` 削除、`#pragma comment` に `_MSC_VER` ガード追加 |
| `ConnectionHash.cpp` | `#pragma comment` に `_MSC_VER` ガード追加 |
| `ControllerMapper.cpp` | `TextColored` フォーマット文字列安全化 (`"%s"` 挿入) |

---

# chore: 不要 #include ディレクティブの一括削除 (21件/19ファイル)

## 2026-03-06

### Removed — 不要インクルード削除

静的解析＋手動検証により、以下の不要な標準ライブラリ/Windows ヘッダーを削除。

#### cli_launcher
| ファイル | 削除ヘッダー |
|----------|-------------|
| `controller/MainController.hpp` | `<vector>` |
| `network_wrapper/ConnectionHash.cpp` | `<chrono>` |
| `ConfigManager.cpp` | `<sstream>` |
| `ConfigManager.hpp` | `<vector>` |
| `test_connection_hash.cpp` | `<cassert>`, `<cstring>` |

#### core_dll/adapter_netplay
| ファイル | 削除ヘッダー |
|----------|-------------|
| `NetplayManager.hpp` | `<windows.h>` |
| `PacketRouter.cpp` | `<iostream>` |
| `SyncCoordinator.cpp` | `<cstring>` |

#### core_dll/adapter_network
| ファイル | 削除ヘッダー |
|----------|-------------|
| `UdpSocket.cpp` | `<iostream>` |
| `UdpSocket.hpp` | `<thread>` |

#### core_dll/adapter_os_hooks
| ファイル | 削除ヘッダー |
|----------|-------------|
| `api_hook/DxHook.cpp` | `<cstdio>` |
| `input/DirectInputHook.cpp` | `<map>` |

#### core_dll/feature_overlay_ui
| ファイル | 削除ヘッダー |
|----------|-------------|
| `ControllerMapper.cpp` | `<windows.h>`, `<cstdio>` |
| `Controller_Ui_Logic.cpp` | `<cmath>` |
| `Controller_Ui_Logic.hpp` | `<cstdint>` |
| `NetplayOverlay.cpp` | `<cstdio>` |

#### core_dll/game_memory_accessor
| ファイル | 削除ヘッダー |
|----------|-------------|
| `state/StateBuffer.hpp` | `<array>` (先行修正) |
| `state/StateBuffer.cpp` | `<iostream>` |

#### core_dll/pure_sync_engine
| ファイル | 削除ヘッダー |
|----------|-------------|
| `RollbackEngine.hpp` | `<cstring>`, `<iostream>` |
