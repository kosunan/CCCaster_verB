# ステージ選択画像

`GRP/BgSelect/` の9枚は旧CCCasterの `OLD/res/GRP/BgSelect/` に同梱されていた、Erkz作成のステージ選択表示修正。55・57・58のプレビュー（stsel_view）と日本語／英語の名称画像（stsel_jp／stsel_en）を、再圧縮せずそのまま保管する。

旧版READMEの説明とMakefileのZIP作成処理に従い、`GRP` は `MBAA.exe` と同じフォルダーに置く。`cccaster_B` 内へ置いてもゲームの相対パスから読み込まれない。元ゲームの外部素材読込みを使うため、画像用のDLLフックは不要。

CMakeが `build/bin/game-assets/` へコピーし、9枚のSHA-256マニフェストを生成する。テスト環境はルートdeploy、配布ZIPはrelease/package.ps1で同じ成果物から配置する。
