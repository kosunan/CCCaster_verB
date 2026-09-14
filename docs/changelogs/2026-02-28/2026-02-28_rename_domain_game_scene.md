# refactor: domain_scene → domain_game_scene フォルダ名変更

## 変更概要
`src/core_dll/domain_scene/` → `src/core_dll/domain_game_scene/` にフォルダ名を変更。

## 変更内容
- `git mv` でフォルダ名変更
- 全 `.cpp` / `.hpp` ファイルの `#include` パスを一括置換
- `CMakeLists.txt` のソースファイルパスを更新
