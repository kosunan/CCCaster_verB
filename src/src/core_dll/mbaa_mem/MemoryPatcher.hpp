#pragma once
// ============================================================================
// MemoryPatcher — プロセスメモリ操作の汎用ユーティリティ (Header-Only)
//
// 【設計思想】
//   DLLインジェクションにおけるメモリ読み書き・命令パッチングの
//   プリミティブ操作を安全に抽象化するユーティリティクラス。
//   このクラスは「どのアドレスに何を書くか」という知識を持たず、
//   純粋な「HOW（どう書くか）」のみを提供する。
//
//   具体的なアドレスやパッチ内容は Domain 層 (GameHooks等) が決定し、
//   本クラスのメソッドに引数として渡す。
//
// 【提供する機能】
//   1. ReadMemory<T>   — 同一プロセス内のメモリ読み取り
//   2. WriteMemory<T>  — 同一プロセス内のメモリ書き込み
//   3. NopPatch        — 指定アドレスの命令を NOP (0x90) で上書き
//   4. WritePatch      — 指定アドレスに任意バイト列を上書き
//
// 【安全性】
//   - VirtualProtect で一時的に PAGE_EXECUTE_READWRITE に変更し、
//     書き込み後に元のプロテクションを復元する。
//   - 不正アドレスへのアクセスは Windows SEH で保護される前提
//     （呼び出し側が __try/__except で囲むこと）。
//
// 【Core層の原則】
//   このファイルは MBAA 固有のアドレスを一切含まない。
//   他のゲーム（GGXX、UNI等）でもそのまま使用可能。
//
// 【使用例】
//   // NOP パッチ（ゲームの入力クリアループを無効化）
//   MemoryPatcher::NopPatch(0x41F098, 2);
//
//   // メモリ読み取り
//   uint32_t gameMode = MemoryPatcher::ReadMemory<uint32_t>(0x54EEE8);
//
//   // メモリ書き込み
//   MemoryPatcher::WriteMemory<uint32_t>(0x74D598, 101);
// ============================================================================

#include <cstdint>
#include <cstring>
#include <windows.h>

namespace cccaster::core::memory {

class MemoryPatcher {
  public:
    // ================================================================
    // ReadMemory — 同一プロセス内のメモリ読み取り
    // ================================================================
    /// @tparam T     読み取る型 (uint8_t, uint32_t, float 等)
    /// @param  addr  読み取り先のアドレス
    /// @return       アドレスに格納されている値
    /// @note   DLLインジェクション環境では reinterpret_cast で直接アクセス可能。
    ///         プロセス外アクセス (ReadProcessMemory) は不要。
    template <typename T> static T ReadMemory(uintptr_t addr) {
        return *reinterpret_cast<T *>(addr);
    }

    // ================================================================
    // WriteMemory — 同一プロセス内のメモリ書き込み
    // ================================================================
    /// @tparam T     書き込む型
    /// @param  addr  書き込み先のアドレス
    /// @param  value 書き込む値
    /// @note   データ領域 (.data, .bss) への書き込みは VirtualProtect 不要。
    ///         コード領域 (.text) への書き込みには NopPatch / WritePatch を使う。
    template <typename T> static void WriteMemory(uintptr_t addr, T value) {
        *reinterpret_cast<T *>(addr) = value;
    }

    // ================================================================
    // NopPatch — 指定アドレスの命令を NOP (0x90) で上書き
    // ================================================================
    /// @param  addr  パッチ先の命令アドレス (.text セクション内)
    /// @param  size  NOP で埋めるバイト数 (元の命令長に合わせる)
    /// @return true  パッチ成功
    /// @return false VirtualProtect 失敗
    ///
    /// 【処理フロー】
    ///   1. VirtualProtect で PAGE_EXECUTE_READWRITE に変更
    ///   2. memset(addr, 0x90, size) で NOP 埋め
    ///   3. VirtualProtect で元のプロテクションに復元
    ///
    /// 【使用場面】
    ///   - ゲームエンジンの入力クリアループの無効化
    ///   - チェック処理のバイパス
    static bool NopPatch(uintptr_t addr, size_t size) {
        DWORD oldProtect;
        if (!VirtualProtect(reinterpret_cast<void *>(addr), size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            return false;
        }
        memset(reinterpret_cast<void *>(addr), 0x90, size);
        VirtualProtect(reinterpret_cast<void *>(addr), size, oldProtect, &oldProtect);
        return true;
    }

    // ================================================================
    // WritePatch — 指定アドレスに任意バイト列を上書き
    // ================================================================
    /// @param  addr  パッチ先のアドレス
    /// @param  data  書き込むバイト列のポインタ
    /// @param  size  書き込むバイト数
    /// @return true  パッチ成功
    /// @return false VirtualProtect 失敗
    ///
    /// 【使用場面】
    ///   - JMP 命令の書き換え（関数フック）
    ///   - コード領域への定数埋め込み
    static bool WritePatch(uintptr_t addr, const void *data, size_t size) {
        DWORD oldProtect;
        if (!VirtualProtect(reinterpret_cast<void *>(addr), size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            return false;
        }
        memcpy(reinterpret_cast<void *>(addr), data, size);
        VirtualProtect(reinterpret_cast<void *>(addr), size, oldProtect, &oldProtect);
        return true;
    }

    // ================================================================
    // ZeroMemoryRegion — 指定アドレスのメモリをゼロクリア
    // ================================================================
    /// @param  addr  クリア先のアドレス
    /// @param  size  クリアするバイト数
    /// @return true  成功
    ///
    /// 【使用場面】
    ///   - キーボードマップの無効化（物理キーボード入力の遮断）
    static bool ZeroMemoryRegion(uintptr_t addr, size_t size) {
        DWORD oldProtect;
        if (!VirtualProtect(reinterpret_cast<void *>(addr), size, PAGE_READWRITE, &oldProtect)) {
            return false;
        }
        memset(reinterpret_cast<void *>(addr), 0, size);
        VirtualProtect(reinterpret_cast<void *>(addr), size, oldProtect, &oldProtect);
        return true;
    }
};

} // namespace cccaster::core::memory
