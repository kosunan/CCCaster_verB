fix(overlay): F4 → キー設定操作でクラッシュする問題を修正 (E-7)

## fix(overlay): F4 → キー設定操作でクラッシュする問題を修正 (E-7)

**日時**: 2026-02-27  
**担当**: AI 作業者C  
**ビルド確認**: ✅ `[100%] Built target cccaster_hook`

---

## クラッシュ原因（3つ特定）

| # | 原因 | 修正 |
|---|---|---|
| 1 | `devices[g_p1JoyId]` 範囲外アクセス — JoyID≠vectorインデックス | ID検索ヘルパーに変更 |
| 2 | F4 の二重処理 — WndProc + ProcessBindingInput 両方で処理 | ProcessBindingInput 側を削除 |
| 3 | キャラセレ外での F4 トグル → 状態不整合 | GameMode ガード追加 |

---

## 変更内容

### [MODIFY] `src/domain/ui/ControllerMapper.cpp`

- `DrawDeviceSelectionTable()`: `devices[g_p1JoyId]`/`devices[g_p2JoyId]` の添え字アクセスを `findDevName()` ラムダによるID逆引きに変更（範囲外アクセス防止）
- `ProcessBindingInput()`: F4 キャンセル処理ブロック (旧L186-201) を削除。F4 による閉じは `OnMappingInput()` に一元化
- `ResetBindingState()`: 新規追加。バインド途中 (g_p1/p2OverlayPosition > 0) の状態を安全にクリア

### [MODIFY] `src/domain/ui/ControllerMapper.hpp`

- `ResetBindingState()` を public static メソッドとして宣言追加

### [MODIFY] `src/domain/ui/NetplayOverlay.cpp`

- `OnMappingInput()`: ウィンドウ閉じ時に `ControllerMapper::ResetBindingState()` を呼び出し

### [MODIFY] `src/core/input/InputHook.cpp`

- `#include "domain/game_constants/MbaaConstants.hpp"` 追加
- F4 処理に `CC_GAME_MODE_ADDR != CC_GAME_MODE_CHARA_SELECT` ガードを追加（キャラセレ画面以外では F4 を無視）
