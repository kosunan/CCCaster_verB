fix(dll): HookLog の cccaster_hook_log.txt 出力先を DLL モジュールパス基準に変更

## fix(dll): HookLog の cccaster_hook_log.txt 出力先を DLL モジュールパス基準に変更

**日時**: 2026-02-27  
**担当**: AI 作業者C  
**ビルド確認**: ✅ `[100%] Built target cccaster_hook`

---

## 問題

`cccaster_hook_log.txt` がカレントディレクトリ（`_TEST_MBAACC\`）に出力されており、  
`cccaster\` サブフォルダ内ではなくゲームルートに混在していた。

---

## 変更内容

### [MODIFY] `src/dll/dllmain.cpp`

- `g_hModule` をグローバルに追加（`static HMODULE g_hModule = nullptr;`）
- `DllMain(DLL_PROCESS_ATTACH)` の冒頭で `g_hModule = hModule` を設定
- `HookLog()` 内で `GetModuleFileNameA(g_hModule, ...)` を使い、DLL 自身のフォルダパスを取得
- ログパスを `<DLL フォルダ>\cccaster_hook_log.txt` として構築

```diff
+static HMODULE g_hModule = nullptr;
 
 void HookLog(const char* msg) {
+    char dllPath[MAX_PATH] = {};
+    if (g_hModule) {
+        GetModuleFileNameA(g_hModule, dllPath, MAX_PATH);
+        char* lastSlash = strrchr(dllPath, '\\');
+        if (lastSlash) *(lastSlash + 1) = '\0';
+    }
+    std::string logPath = std::string(dllPath) + "cccaster_hook_log.txt";
-    FILE* fp = fopen("cccaster_hook_log.txt", "a");
+    FILE* fp = fopen(logPath.c_str(), "a");
     ...
+case DLL_PROCESS_ATTACH:
+    g_hModule = hModule;
     DisableThreadLibraryCalls(hModule);
```

---

## 結果

| ログ出力先 | 変更前 | 変更後 |
|---|---|---|
| `_TEST_MBAACC\cccaster_hook_log.txt` | ✅ ここに出ていた | ❌ 出なくなった |
| `_TEST_MBAACC\cccaster\cccaster_hook_log.txt` | ❌ 出ていなかった | ✅ ここに出るようになった |

---

## デプロイ

`build\bin\libcccaster_hook.dll` → `_TEST_MBAACC\cccaster\libcccaster_hook.dll` にコピー完了。
