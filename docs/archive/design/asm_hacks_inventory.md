# AsmHacks ユーティリティ一覧

| 項目 | 内容 |
|---|---|
| 元ファイル | `prototype_workspace/2_InjectedCore/AsmHacks/DllAsmHacks.hpp/.cpp` |
| 総行数 | hpp: 605行, cpp: 615行 |

---

## カテゴリ別ハック一覧

### 🔴 最優先（シーン業務処理に必須）

| # | ハック名 | 種別 | 用途 | 使用Scene |
|---|---|---|---|---|
| 1 | `hookMainLoop` | AsmList | ゲームのメインメッセージループに DLL callback を挿入 | 全Scene（SceneRunner起点） |
| 2 | `hijackMenu` | AsmList | メニューカーソル位置(`currentMenuIndex`)取得 + 確認入力ゲート(`menuConfirmState`) | Rematch, CharaSelect |
| 3 | `hijackControls` | AsmList | ジョイスティック/キーボード入力を無効化（DLLが入力を完全制御） | 全Scene |
| 4 | `detectRoundStart` | AsmList | ラウンド開始カウンタ(`roundStartCounter`)をインクリメント | InGame |
| 5 | `hijackIntroState` | Asm | introState=0 への自動リセットを無効化（ロールバック用） | InGame |

### 🟡 重要（機能拡張に必要）

| # | ハック名 | 種別 | 用途 | 使用Scene |
|---|---|---|---|---|
| 6 | `filterRepeatedSfx` | AsmList | SFX重複再生フィルタ（`sfxFilterArray`で制御） | InGame（ロールバック中） |
| 7 | `muteSpecificSfx` | AsmList | 特定SFXのミュート（`sfxMuteArray`で制御） | InGame（ロールバック中） |
| 8 | `hijackEscapeKey` | Asm | Escキーの終了機能を制御（`enableEscapeToExit`フラグ） | 全Scene |
| 9 | `disableFpsLimit` | Asm | FPS上限解除（FastForward用） | Loading, CharaSelect |
| 10 | `disableFpsCounter` | Asm | FPSカウンタ更新を無効化 | 全Scene |

### 🟢 ゲームモード制御

| # | ハック名 | 種別 | 用途 |
|---|---|---|---|
| 11 | `forceGotoVersus` | Asm | Versusモードへ強制遷移 |
| 12 | `forceGotoVersusCPU` | Asm | VersusCPUモードへ強制遷移 |
| 13 | `forceGotoTraining` | Asm | Trainingモードへ強制遷移 |
| 14 | `forceGotoReplay` | Asm | Replayモードへ強制遷移 |
| 15 | `multiWindow` | Asm | 多重起動チェックをスキップ |

### 🔵 描画・カラー・ステージ

| # | ハック名 | 種別 | 用途 |
|---|---|---|---|
| 16 | `hijackCharaSelectColors` | Asm | キャラセレ中のカラーデータロード時コールバック挿入 |
| 17 | `hijackLoadingStateColors` | AsmList | Loading中のカラーデータロード時コールバック挿入 |
| 18 | `enableDisabledStages` | AsmList | 無効ステージの有効化 + 龍儀ステージBGMループ修正 |
| 19 | `fixBossStageSuperFlashOverlay` | Asm | ボスステージの超必エフェクト修正 |
| 20 | `disableHealthBars` | AsmList | ゲームUIの非表示（HP/ガードバー/タイマー等） |
| 21 | `hookPresentCaller` | AsmList | D3D Present呼び出しフック（オーバーレイ描画用） |
| 22 | `addExtraDraws` | AsmList | 追加描画コールバック |
| 23 | `addExtraTextures` | AsmList | 追加テクスチャロードコールバック |
| 24 | `loadCustomPalettesAsm` | AsmList | カスタムパレットロードコールバック |

### ⚪ リプレイ

| # | ハック名 | 種別 | 用途 |
|---|---|---|---|
| 25 | `saveReplay` | AsmList | リプレイ保存名のコピー |
| 26 | `detectAutoReplaySave` | Asm | 自動リプレイ保存状態の検出 |
| 27 | `disableTrainingMusicReset` | Asm | トレーニングモードBGMリセット無効化 |
| 28 | `disableScreenshot` | Asm | ScrollLock自動スクリーンショット無効化 |

---

## 主要グローバル変数

| 変数名 | 型 | 用途 |
|---|---|---|
| `currentMenuIndex` | `uint32_t` | メニューカーソル位置（hijackMenuで更新） |
| `menuConfirmState` | `uint32_t` | 確認入力ゲート（0=ブロック, 1=検出通知, ≥2=通過） |
| `roundStartCounter` | `uint32_t` | ラウンド開始カウンタ |
| `enableEscapeToExit` | `uint8_t` | Escキー制御フラグ |
| `sfxFilterArray` | `uint8_t[CC_SFX_ARRAY_LEN]` | SFX再生カウンタ（重複防止） |
| `sfxMuteArray` | `uint8_t[CC_SFX_ARRAY_LEN]` | SFXミュートフラグ |
| `replayName` | `char*` | リプレイファイル名 |
| `autoReplaySaveStatePtr` | `uint32_t*` | 自動リプレイ保存状態ポインタ |
| `currentColorTablePtr` | `uint32_t*` | カラーテーブルポインタ |
| `numLoadedColors` | `uint32_t` | ロード済みカラー数 |

---

## 基盤ユーティリティ（マクロ/構造体）

| 名前 | 用途 |
|---|---|
| `Asm` 構造体 | アドレス+バイト列+バックアップ。`write()` / `revert()` で適用/復元 |
| `AsmList` | `vector<Asm>` — 複数パッチの一括適用 |
| `WRITE_ASM_HACK()` | エラーチェック付きパッチ適用マクロ |
| `INLINE_DWORD()` | 32bitアドレスをリトルエンディアンバイト列に展開 |
| `PATCHJUMP()` / `PATCHCALL()` | 相対ジャンプ/コールのパッチ生成 |
| `DISABLECALL()` | 5バイトNOP（CALL命令無効化） |
| `PUSH_ALL` / `POP_ALL` | 全レジスタ退避/復元 |
| `emitCall()` / `emitJump()` | インラインASM用のcall/jmpエミッタ |
| `memwrite()` | VirtualProtect付きメモリ書き込み |
