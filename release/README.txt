CCCaster verB

対象: MBAACC Ver.1.07 Rev.1.4.0（カニファン版）、Windows 32bit。
通信版: 10。Steam版とのクロスプレイには対応していません。

更新手順:
1. MBAAとCCCasterを終了し、既存のCLI・GUI・DLLをバックアップします。
2. ZIP内のcccaster_BフォルダーにあるCCCaster_B.exe、CCCaster_B_GUI.exe、
   libcccaster_hook.dllを、ゲーム側の既存cccaster_Bフォルダーへまとめて配置します。
3. ゲーム本体、cccaster.ini、旧INI、コントローラー設定は保持してください。
   cccaster_B/cccaster_Bという二重フォルダーにしないでください。
4. CCCaster_B_GUI.exeを直接起動してください。対戦する双方を同じ配布版へ更新します。
   旧名称のEXEを使うショートカットは、新しい固定名のEXEへ変更してください。

既知の制約:
IPv6ホールパンチにはIPv4経由の宛先交換が必要です。
別PC・実回線の長時間対戦、全キャラ、物理コントローラーの総合確認は未完了です。
リプレイの通常試合全編の再生一致は追加確認対象です。
詳細はリポジトリのdocs/OPEN_ISSUES.mdを参照してください。

SHA256SUMS.txtには同梱する3バイナリのSHA-256を記載しています。
