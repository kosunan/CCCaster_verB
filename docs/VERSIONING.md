# 製品名とバージョン管理

製品名はCCCaster verB、作業フォルダーとoriginはCCCaster_verBに固定する。
CLIはCCCaster_B.exe、GUIはCCCaster_B_GUI.exe、DLLはlibcccaster_hook.dll。
配置先cccaster_Bと設定cccaster.iniもバージョンから独立する。

更新時はルートのVERSIONだけを変更し、build.batで構成・ビルド・CTestを実行する。
CMakeプロジェクトとCLI・GUI・DLLの表示はVERSIONから生成する。
release/package.ps1はVERSIONからZIP名を生成する。同名ZIPがある場合は上書きせず停止する。

旧cccaster_v10.iniは新設定がない場合だけ複製して引き継ぐ。新設定を優先し、旧設定は削除しない。
通信版10、IPC識別子、接続コードのソルトは互換性のため独立管理する。
過去リリースURLと日付付き記録の名称は当時のまま保持する。
