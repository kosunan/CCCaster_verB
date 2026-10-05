# CCCaster verB

MBAACC Ver.1.07 Rev.1.4.0（非Steam版・32bit）用の対戦・観戦・トレーニング支援ツールです。製品バージョンは [VERSION](VERSION) で管理します。

## 利用する

CLI は `CCCaster_B.exe`、GUI は `CCCaster_B_GUI.exe`。配布物の配置・更新は [配布手順](release/README.txt)、操作と確認済みの範囲は [現行仕様](docs/CURRENT_STATE.md)、制約は [未解決事項](docs/OPEN_ISSUES.md) を参照してください。更新時は CLI・GUI・DLL をまとめて差し替え、INI・コントローラ設定・リプレイを保持します。

GUIはRmlUi 6.3で構成し、描画ライブラリ・FreeType・日本語フォント・画面素材をEXEへ組み込みます。**3バイナリと`gui-licenses/`を配置**してください。WebView2・Python・利用者のブラウザー・CDNへの依存はありません。Windows 10/11を対象とし、F8または表示メニューでCPU描画（WARP）へ切り替えられます。[構成・検証範囲](docs/design/2026-10-04_rmlui_gui.md)。

GUIから公開／非公開マッチング、観戦、Training、リプレイ再生を起動できます。マッチングは待受と申し込みを両立し、受信側の手動承諾で成立します。公開一覧は対戦中の人も残して20名ずつ表示します。[操作と実装範囲](docs/design/2026-10-04_matching_implementation.md)。F4でコントローラを設定します。対戦は両者が同じ更新版を使用してください。ランク／勝敗報告サーバーは凍結中です。

マッチングの**6文字コード**は取消まで同じで、対戦終了後はそのコードで待受へ戻ります。大小文字は区別しません。観戦も対戦者どちらかのコードで開始前から待機でき、双方の許可が必要です（初期値ON）。接続成立後はLAN・IPv6・IPv4から直接UDP接続します。GUIの優先指定、従来の直接接続・手動コード交換・オフラインLAN探索も維持しています。設定・CLI例は [P2P接続](docs/design/2026-10-04_p2p_connection.md)、観戦のTCP条件は [観戦待機](docs/design/2026-10-04_spectator_standby.md) を参照してください。

## 開発する

作業ルートは `I:/work_space/CCCaster_verB`。ルールは [AGENTS.md](AGENTS.md) と [src/AGENTS.md](src/AGENTS.md)、検証の入口は [開発手順](docs/DEVELOPMENT.md) です。構造を図から読む場合は [Whiteboardの試用手順](docs/WHITEBOARD.md) を参照してください。

```powershell
.\build.bat
python -X utf8 -m unittest discover -s src/src/harness -p test_*.py
```

`build.bat` は 32bit Release ビルドと CTest を実行します。CMake 入力は `src/`、成果物は `build/bin/` です。

| 場所 | 用途 |
|---|---|
| `src/src/` | 製品ソース・単体テスト・同期harness |
| `src/CMakeLists.txt` | CMake の入口 |
| `.github/workflows/` | GitHub CI |
| `docs/` | 現行仕様、課題、設計・検証記録、変更履歴 |
| `test/runtime/`・`test/logs/` | 隔離した実ゲームと検証ログ（ゲーム・ログはGit管理外） |
| `release/` | 配布手順・マニフェスト・生成した配布物 |
| `archive/` | 旧環境の読み取り専用保全物 |

旧 `CCCaster_v10` のゲーム・設定・ログは保全し、ビルド元・差し替え元には使用しません。Steam版は別プロジェクトで管理します。
