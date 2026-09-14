# refactor(domain): SleepFrame を SceneRunner ループ先頭に集約 (E-8)

## 変更内容

### `src/domain/session/SceneRunner.cpp`
- メインループ (`while(running)`) の冒頭に `GC::SleepFrame()` を1箇所配置
- Gate 1 (ロールバック)、Gate 2 (高速起動)、default case からの SleepFrame を除去
- ProcessRollbackGate() 内の SleepFrame を除去

### `src/domain/scene/SceneCharaSelect.cpp`
- 3箇所の `GC::SleepFrame()` を除去 (L163, L189, L247)

### `src/domain/scene/SceneLoading.cpp`
- 4箇所の `GC::SleepFrame()` を除去 (L96, L106, L154, L175)

### `src/domain/scene/SceneInGame.cpp`
- 5箇所の `GC::SleepFrame()` を除去 (L83, L96, L132, L181, L273)

### `src/domain/scene/SceneRematch.cpp`
- 3箇所の `GC::SleepFrame()` を除去 (L286, L295, L334)

### `docs/ISSUE_TRACKER.md`
- E-5 (FPS異常) を ✅修正済みに更新
- E-8 (SleepFrame分散問題) を新規追加・🔧対応中に設定

## 設計方針
各 Scene::Update() は「1回の呼び出しで1フレーム分の業務処理のみ行い return する」設計。
フレームタイミング制御は SceneRunner のメインループ先頭1箇所に集約し、
高速モード(skipMode)と通常モード(~16.6ms スリープ)の分岐も VirtualClock 内部に委譲する。

## 動作確認
- `cccaster_hook` ビルド [100%] 成功
