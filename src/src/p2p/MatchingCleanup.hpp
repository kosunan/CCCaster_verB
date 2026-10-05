#pragma once
#include "p2p/Protocol.hpp"
#include <memory>
#include <filesystem>
#include <functional>

namespace cccaster::matching {
// 署名・暗号化済みの取消だけを預ける。待機中のHTTP通信や秘密鍵の受渡しは行わない。
class CleanupGuard {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit CleanupGuard(const std::filesystem::path& directory = {});
    ~CleanupGuard();
    bool Arm(p2p::Json payload);
    void Defer(int64_t until);
    void Disarm();
};
// 同じEXEの非表示モード。親の書込みpipeが閉じたら残った取消を送信して終了する。
int RunCleanupWorker(uintptr_t input, uintptr_t acknowledgment, uintptr_t lease = 0);
struct CleanupRecovery { unsigned pending = 0, completed = 0; };
// 使用中の登録を除き、同じ保存先・通知サーバーの未送信分だけ再送する。
CleanupRecovery RecoverCleanup(const std::filesystem::path& directory, const std::string& server,
    const std::function<bool(const std::string&, const std::string&)>& post, int64_t& nextPost);
}
