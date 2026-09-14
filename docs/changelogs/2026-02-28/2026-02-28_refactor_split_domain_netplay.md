# refactor: domain_netplay を4フォルダに責務分割

## 変更概要
`domain_netplay/`(23ファイル) を責務ごとに4フォルダに分割。DEAD CODE を削除。

## 新フォルダ構成
- `domain_sync/` — RollbackEngine, InputFilter, RemoteInputQueue, InputsContainer, NetplayState (7ファイル)
- `domain_timer/` — VirtualClock, TimeSynchronizer, TimeHooks (6ファイル)
- `domain_network/` — GameHooks, PacketRouter (4ファイル)
- `domain_overlay/` — NetplayOverlay, ControllerMapper (4ファイル)

## 削除
- `RedundantProtocol.cpp/hpp` — DEAD CODE (E-11/E-12で完全代替)
- `StateBuffer.hpp` から RedundantProtocol.hpp への不要 include を除去

## 影響範囲
- 全ソースの `#include` パスを一括置換
- `CMakeLists.txt` のソースファイルパスを更新
