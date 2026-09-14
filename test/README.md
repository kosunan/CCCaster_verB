# 実ゲーム3窓テスト

ゲーム実体はruntime/MBAACC_1（ホスト）、MBAACC_2（対戦相手）、MBAACC_3（観戦）。
各コピーのcccaster_Bへ最新CLI・GUI・DLLを配置する。旧環境とは独立したコピー。

ルートで次を実行する。指定TCP/UDPポートが空いていることを確認する。

```powershell
./src/src/harness/run_spectator_real.ps1 -Seconds 55 -Port 18964 -JoinDelay 8 -UseCode
python -X utf8 src/src/harness/compare_rollback_pair.py test/logs/spectator_日時/pair
python -X utf8 src/src/harness/compare_spectator.py test/logs/spectator_日時
```

既定は15〜25ms・5%損失の脚本入力試験。今回起動したゲームのみ終了する。
GitHub CIは実行しない。物理操作・別PCの疎通・実画面の目視とは別の確認。
