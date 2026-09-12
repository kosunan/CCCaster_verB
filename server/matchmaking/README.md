# マッチングサーバー試作

**2026-09-12：ユーザー指定で凍結。** ランク／マッチング画面はGUIから除外した。Cloudflareアカウントの準備・サーバー公開は当面不要。以下は再開時のために保存した試作の記録であり、現在の作業手順ではない。先に旧版の機能と観戦を復元する。

Cloudflare Workers Free + SQLite Durable Objects向け。公開サーバーはまだ作成していない。GUIとのAPI接続、実ゲームからの勝敗報告、ゲーム内での選択制限も未接続。通常のCLI対戦は変更しない。

## ローカル確認

```powershell
cd server/matchmaking
npm ci
npm test
$env:WRANGLER_SEND_METRICS='false'
node smoke.mjs
```

`smoke.mjs` は試験専用アカウントでローカルWorkersランタイムを起動し、認証・キュー・双方の結果報告・加点をHTTPで確認する。公開や課金プラン変更は行わない。

## 合意済みルール

- ランクは2試合先取（最大3試合）。試合内の通常ラウンド数は変更しない。
- 全員1段から開始。段位ポイントを貯めて昇格する。
- 同一セット中は双方ともキャラクターを固定。直前の勝者はスタイルも固定。直前の敗者だけがスタイルを変えられる。
- 試作の配点は `core.mjs` のRULESで仮設定: セット勝利100ポイント、300ポイントで1段昇格、敗北減点なし。配点は未確定。
- カジュアルは別キューで段位変動なし。試作では1試合でマッチ終了。

## API

すべてPOSTのJSON。Authorizationは `Bearer <個人トークン>`。対戦用接続コードはマッチ相手だけに返す。

| パス | 内容 |
|---|---|
| /me | 1段からの段位・累積ポイント・次段までの必要ポイント |
| /queue/join | mode=ranked/casual、protocol=10、codeを送信。近い段位優先、カジュアルは先着 |
| /queue/status | 相手決定の確認と90秒の待機期限更新 |
| /queue/cancel | 待機列から退出。決定済みの対戦を勝敗へ変換しない |
| /match | idを指定し、自分のマッチだけを確認 |
| /match/report | id、game（0始まり）、winner（0/1）、selections（両者のcharacter/style）を報告 |

両側の勝敗・キャラ・スタイルが一致した試合だけ確定する。結果の再送は重複加点しない。不一致・30分経過は加点せず終了。引き分け・切断時の扱いは今後詰める。片側の報告だけでは段位を更新しない。

## Cloudflareアカウント準備後

アカウントはユーザーが用意する。無料プランで試験し、Workers Paidへ自動変更しない。アカウントへのログイン、公開URLの確定、招待アカウント準備が必要。

試験は招待制。`PLAYERS_JSON` secretに `SHA-256(個人トークン)` をキー、`p_`から始まるプレイヤーIDを値とするJSONを登録する。十分長いランダムトークンを個別に発行し、チャットやGitへ貼らない。スモーク試験の既知トークンを公開環境で使わない。

```powershell
npx wrangler login
npx wrangler secret put PLAYERS_JSON
npx wrangler deploy
```

再開する場合、公開前にGUI/API接続・勝敗確定イベント・ゲーム内の選択制限とその実機検証が必要。現在はGUIに対戦検索を表示していない。サーバーだけ公開しても実ゲームのランク戦は成立しない。

## 試作の制約

招待人数を限定した小規模実験向け。双方のクライアントが虚偽を報告する談合や改造クライアントの検出は実装していない。結果一致は正しい実ゲーム結果の証明ではない。一般公開には認証・結果検証・レート制限・切断裁定を追加する。

サーバーは待機列・段位管理用で、UDP中継ではない。既存P2Pで繋がらないNAT環境をこのサーバーだけで解決することはできない。

無料枠は無制限ではない。Workers FreeでSQLite Durable Objectsを利用でき、DO要求は1日100,000、超過時は失敗する。ポーリング間隔と利用人数を決めてから実運用する。

- [Cloudflare公式料金表](https://developers.cloudflare.com/durable-objects/platform/pricing/)
- [Workers公式料金表](https://developers.cloudflare.com/workers/platform/pricing/)
- 比較候補: [Supabase公式料金表](https://supabase.com/pricing)

## 2026-09-11の確認結果

- GUIの32bitビルド、日本語のランク／カジュアル画面を確認。
- サーバー単体8テスト成功。ローカルWorkersランタイムでHTTP認証・待機列・組合せ・双方報告・加点を確認。
- 互換日をランタイムが対応する2025-04-01に固定。開発ランタイムとCLIはpackage-lock.jsonで固定。
- 根拠: `build_logs/gui_20260911/build_matchmaking.log`、`matchmaking_tests.log`、`matchmaking_http.log`。
- Cloudflareアカウントはユーザーがこれから準備。公開および実ゲームでのランク戦は未実施。
